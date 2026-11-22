#!/usr/bin/env bash
# Build the three deployment branches, upload each gamex86.dll to the matching
# game server, and restart the shared service:
#
#   master       -> winserver1/Base/gamex86.dll  (stable features)
#   unstable     -> winserver2/Base/gamex86.dll  (stable + unstable)
#   experimental -> winserver3/Base/gamex86.dll  (all features)
#
# There are no options. Running this script:
#   1. fetches origin,
#   2. builds each branch in a detached worktree under .worktrees/,
#   3. backs up the current DLL and uploads the new one,
#   4. verifies the remote checksum,
#   5. restarts sof_winserver1 (which controls all three servers).
#
# Uploads use a temporary file plus rename, so a running server never sees a
# partial DLL. The restart prompts for the remote sudo password when run from
# a terminal.
#
# Environment overrides:
#   REMOTE_HOST             SSH host (default: plowsof)
#   REMOTE_BASE             remote directory containing winserver1..3
#                           (default: the remote login home)
#   BUILD_TYPE              Release or Debug (default: Release)
#   PYTHON3                 Python with PyYAML (default: /usr/bin/python3)
#   SOFPLUS_CURSOR_RULES    command catalog repo
#                           (default: ../sofplus-cursor-rules)
#   SERVICE_PREFIX          systemd unit prefix (default: sof_)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BRANCHES=(master unstable experimental)
SERVERS=(winserver1 winserver2 winserver3)

# Each branch selects a different feature profile. The worktrees are detached,
# so build.sh cannot infer the branch from git - pass it explicitly.
profile_for() {
  case "$1" in
    master) printf 'stable' ;;
    *) printf '%s' "$1" ;;
  esac
}

# A branch that predates tiers silently ignores SOF_BUDDY_PROFILE and builds
# the old default feature set. Refuse it instead of deploying the wrong DLL.
ref_has_feature_profiles() {
  git grep -q 'SOF_BUDDY_PROFILE' "$1" -- scripts/build.sh tools/generate_hooks.py 2>/dev/null \
    && git grep -q '^profiles:' "$1" -- src/features/features.yaml 2>/dev/null
}

REMOTE_HOST="${REMOTE_HOST:-plowsof}"
REMOTE_BASE="${REMOTE_BASE:-}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
PYTHON3="${PYTHON3:-/usr/bin/python3}"
RULES_DIR="${SOFPLUS_CURSOR_RULES:-$ROOT/../sofplus-cursor-rules}"
SERVICE_PREFIX="${SERVICE_PREFIX:-sof_}"
RESTART_SERVICE="${SERVICE_PREFIX}winserver1"
WORKTREE_ROOT="$ROOT/.worktrees"
BUILD_ROOT="$ROOT/build/branches"

STAMP="$(date +%Y%m%d-%H%M%S)"

usage() {
  cat <<'EOF'
Usage: scripts/deploy-branches.sh

Fetches origin, builds master/unstable/experimental, backs up and uploads each
gamex86.dll to the matching winserver directory, then restarts sof_winserver1.
EOF
  exit "${1:-1}"
}

die() {
  echo "error: $*" >&2
  exit 1
}

dest_dir_for() {
  if [[ -n "$REMOTE_BASE" ]]; then
    printf '%s/%s/Base' "$REMOTE_BASE" "$1"
  else
    printf '%s/Base' "$1"
  fi
}

shell_quote() {
  printf '%q' "$1"
}

if (( $# > 0 )); then
  case "$1" in
    -h|--help) usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
fi

# Normalise config.
REMOTE_BASE="${REMOTE_BASE%/}"
if [[ "$REMOTE_BASE" == "." ]]; then
  REMOTE_BASE=""
fi
if [[ -d "$RULES_DIR" ]]; then
  RULES_DIR="$(cd "$RULES_DIR" && pwd)"
fi

command -v git >/dev/null 2>&1 || die "git not found"

echo "==> Fetching origin..."
if ! git fetch --prune origin; then
  die "git fetch failed"
fi
git worktree prune

# Resolve every branch before building anything, so a missing branch cannot
# leave a half-deployed set of servers.
sel_branches=()
sel_servers=()
sel_refs=()
sel_commits=()
sel_subjects=()
missing=()
for i in "${!BRANCHES[@]}"; do
  branch="${BRANCHES[$i]}"
  server="${SERVERS[$i]}"

  ref=""
  if git show-ref --verify --quiet "refs/heads/$branch"; then
    ref="refs/heads/$branch"
  elif git show-ref --verify --quiet "refs/remotes/origin/$branch"; then
    ref="refs/remotes/origin/$branch"
  else
    missing+=("$branch")
    continue
  fi

  if ! ref_has_feature_profiles "$ref"; then
    echo "error: [$branch] $ref predates feature profiles." >&2
    echo "       It would ignore the $branch feature profile and deploy the old default set." >&2
    if [[ "$branch" == "master" ]]; then
      echo "       Commit the profile changes on master before deploying." >&2
    else
      echo "       Fast-forward it after committing master:" >&2
      echo "         git branch -f $branch master" >&2
    fi
    exit 1
  fi

  sel_branches+=("$branch")
  sel_servers+=("$server")
  sel_refs+=("$ref")
  sel_commits+=("$(git rev-parse --short=12 "$ref^{commit}")")
  sel_subjects+=("$(git log -1 --format=%s "$ref")")
done

if (( ${#missing[@]} > 0 )); then
  die "missing branch(es): ${missing[*]} (create and push them first)"
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v i686-w64-mingw32-g++ >/dev/null 2>&1 \
  || die "i686-w64-mingw32-g++ not found (install the mingw-w64 i686 toolchain)"
[[ -x "$PYTHON3" ]] || die "Python interpreter not found: $PYTHON3"
"$PYTHON3" -c 'import yaml' >/dev/null 2>&1 \
  || die "$PYTHON3 does not have PyYAML (set PYTHON3 to a suitable interpreter)"

command -v ssh >/dev/null 2>&1 || die "ssh not found"
command -v scp >/dev/null 2>&1 || die "scp not found"

echo "==> Checking destination directories on $REMOTE_HOST..."
for i in "${!sel_branches[@]}"; do
  dest_dir="$(dest_dir_for "${sel_servers[$i]}")"
  if ! ssh "$REMOTE_HOST" "test -d $(shell_quote "$dest_dir")"; then
    die "cannot access $REMOTE_HOST:$dest_dir (SSH failed or directory is missing)"
  fi
done

if [[ ! -d "$RULES_DIR" ]]; then
  echo "warning: $RULES_DIR not found; command catalog will use the fallback list" >&2
fi

mkdir -p "$WORKTREE_ROOT" "$BUILD_ROOT"

sel_dlls=()
sel_shas=()
for i in "${!sel_branches[@]}"; do
  branch="${sel_branches[$i]}"
  ref="${sel_refs[$i]}"
  commit="${sel_commits[$i]}"
  subject="${sel_subjects[$i]}"
  profile="$(profile_for "$branch")"
  worktree="$WORKTREE_ROOT/$branch"
  build_dir="$BUILD_ROOT/$branch"

  echo
  echo "==> [$branch] building $commit [$profile] $subject"

  if [[ -e "$worktree" ]]; then
    if ! git -C "$worktree" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      die "$worktree exists but is not a git worktree; remove it and retry"
    fi
    git -C "$worktree" checkout --detach --force --quiet "$ref"
    git -C "$worktree" reset --hard --quiet "$ref"
    git -C "$worktree" clean -ffdxq
  else
    git worktree add --detach --force --quiet "$worktree" "$ref"
  fi

  mkdir -p "$build_dir"
  build_env=(
    "BUILD_DIR=$build_dir"
    "BUILD_TYPE=$BUILD_TYPE"
    "PYTHON3=$PYTHON3"
    "SOF_BUDDY_PROFILE=$profile"
  )
  if [[ -d "$RULES_DIR" ]]; then
    build_env+=("SOFPLUS_CURSOR_RULES=$RULES_DIR")
  else
    build_env+=("SOFPLUS_CURSOR_RULES=")
  fi

  (
    cd "$worktree"
    env "${build_env[@]}" bash scripts/build.sh
  )

  dll="$build_dir/gamex86.dll"
  [[ -s "$dll" ]] || die "[$branch] build produced no $dll"

  sel_dlls+=("$dll")
  sel_shas+=("$(sha256sum "$dll" | awk '{print $1}')")
  printf '    built %s (%s bytes, sha256 %.12s)\n' \
    "$dll" "$(wc -c < "$dll")" "${sel_shas[$i]}"
done

echo
echo "==> Uploading to $REMOTE_HOST..."
for i in "${!sel_branches[@]}"; do
  branch="${sel_branches[$i]}"
  server="${sel_servers[$i]}"
  dest_dir="$(dest_dir_for "$server")"
  dest="$dest_dir/gamex86.dll"
  backup="$dest.bak.$STAMP"
  tmp="$dest_dir/.gamex86.dll.new.$$"

  echo "==> [$branch] $REMOTE_HOST:$dest"

  ssh "$REMOTE_HOST" \
    "if [ -e $(shell_quote "$dest") ]; then cp -p $(shell_quote "$dest") $(shell_quote "$backup"); fi"

  scp -q "${sel_dlls[$i]}" "$REMOTE_HOST:$tmp"
  ssh "$REMOTE_HOST" "mv -f $(shell_quote "$tmp") $(shell_quote "$dest")"

  remote_sha="$(ssh "$REMOTE_HOST" "sha256sum $(shell_quote "$dest")" | awk '{print $1}')"
  if [[ "$remote_sha" != "${sel_shas[$i]}" ]]; then
    die "[$branch] checksum mismatch after upload to $REMOTE_HOST:$dest"
  fi
done

echo
echo "==> Restarting $RESTART_SERVICE (controls all servers)..."
if [[ -t 0 ]]; then
  # Allocate a remote TTY so sudo can prompt for the password.
  if ! ssh -t "$REMOTE_HOST" "sudo systemctl restart $(shell_quote "$RESTART_SERVICE")"; then
    die "failed to restart $RESTART_SERVICE"
  fi
else
  # No local TTY (CI/pipe): sudo cannot prompt.
  if ! ssh "$REMOTE_HOST" "sudo -n systemctl restart $(shell_quote "$RESTART_SERVICE")"; then
    die "failed to restart $RESTART_SERVICE (run from a terminal to enter the sudo password, or configure passwordless sudo)"
  fi
fi
if ! ssh "$REMOTE_HOST" "systemctl is-active --quiet $(shell_quote "$RESTART_SERVICE")"; then
  die "$RESTART_SERVICE is not active after restart"
fi

echo
echo "==> Installed artifacts on $REMOTE_HOST"
for i in "${!sel_branches[@]}"; do
  printf '    %-12s [%-12s] -> %-11s %s  sha256 %.12s\n' \
    "${sel_branches[$i]}" "$(profile_for "${sel_branches[$i]}")" \
    "${sel_servers[$i]}" \
    "${sel_commits[$i]}" "${sel_shas[$i]}"
done
