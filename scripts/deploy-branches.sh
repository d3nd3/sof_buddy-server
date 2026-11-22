#!/usr/bin/env bash
# Build the three deployment branches and install each gamex86.dll on the
# game server:
#
#   master       -> winserver1/Base/gamex86.dll  (stable features)
#   unstable     -> winserver2/Base/gamex86.dll  (stable + unstable)
#   experimental -> winserver3/Base/gamex86.dll  (all features)
#
# Each branch is built from a detached git worktree under .worktrees/, so the
# checkout you are working in is never touched. Build output lives in
# build/branches/<branch>/.
#
# Uploads write to a temporary file in the destination directory and then
# rename it over gamex86.dll, so a running server never sees a partial DLL.
# The running processes keep using the old DLLs until they are restarted; pass
# --restart to restart sof_winserver1 after all uploads. That service controls
# all three servers, so they all come back together.
#
# Examples:
#   ./scripts/deploy-branches.sh
#   ./scripts/deploy-branches.sh --only master
#   ./scripts/deploy-branches.sh --only master,unstable --restart
#   ./scripts/deploy-branches.sh --dry-run
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

ONLY=()
FETCH=1
SKIP_MISSING=0
BUILD_ONLY=0
DRY_RUN=0
RESTART=0
BACKUP=0
STAMP="$(date +%Y%m%d-%H%M%S)"

usage() {
  cat <<'EOF'
Usage: scripts/deploy-branches.sh [options]

Builds master/unstable/experimental and uploads each build to the matching
winserver directory over SSH.

Options:
  --only BRANCH[,BRANCH]  deploy only the named branches (default: all three)
  --host HOST             SSH host (default: plowsof, or $REMOTE_HOST)
  --remote-base DIR       directory containing winserver1..3 on the remote host
                          (default: remote home, or $REMOTE_BASE)
  --skip-missing          keep going when a branch does not exist
  --no-fetch              do not run "git fetch origin" first
  --debug                 build CMAKE_BUILD_TYPE=Debug instead of Release
  --restart               restart sof_winserver1 after all uploads; that
                          service brings up all three servers together
  --backup                save the previous gamex86.dll as
                          gamex86.dll.bak.YYYYMMDD-HHMMSS before overwriting
  --build-only            build the DLLs but do not upload them
  --dry-run               show what would be built and where, then exit
  -h, --help              show this help

Branches are built from their local tip when it exists, otherwise from
origin/<branch>. Committed changes only: uncommitted files are not included.
EOF
  exit "${1:-1}"
}

die() {
  echo "error: $*" >&2
  exit 1
}

branch_selected() {
  if (( ${#ONLY[@]} == 0 )); then
    return 0
  fi
  local b
  for b in "${ONLY[@]}"; do
    if [[ "$b" == "$1" ]]; then
      return 0
    fi
  done
  return 1
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --only)
      [[ $# -ge 2 ]] || usage
      IFS=',' read -r -a ONLY <<< "$2"
      shift 2
      ;;
    --host)
      [[ $# -ge 2 ]] || usage
      REMOTE_HOST="$2"
      shift 2
      ;;
    --remote-base)
      [[ $# -ge 2 ]] || usage
      REMOTE_BASE="$2"
      shift 2
      ;;
    --skip-missing) SKIP_MISSING=1; shift ;;
    --no-fetch) FETCH=0; shift ;;
    --debug) BUILD_TYPE=Debug; shift ;;
    --restart) RESTART=1; shift ;;
    --backup) BACKUP=1; shift ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage 0 ;;
    *)
      echo "unknown option: $1" >&2
      usage
      ;;
  esac
done

# Normalise config.
REMOTE_BASE="${REMOTE_BASE%/}"
if [[ "$REMOTE_BASE" == "." ]]; then
  REMOTE_BASE=""
fi
if [[ -d "$RULES_DIR" ]]; then
  RULES_DIR="$(cd "$RULES_DIR" && pwd)"
fi

if (( ${#ONLY[@]} > 0 )); then
  for b in "${ONLY[@]}"; do
    known=0
    for candidate in "${BRANCHES[@]}"; do
      if [[ "$b" == "$candidate" ]]; then
        known=1
      fi
    done
    (( known )) || die "unknown branch '$b' (choose from: ${BRANCHES[*]})"
  done
fi

command -v git >/dev/null 2>&1 || die "git not found"

if (( FETCH )); then
  echo "==> Fetching origin..."
  if ! git fetch --prune origin; then
    die "git fetch failed (use --no-fetch to build existing refs)"
  fi
fi
git worktree prune

# Resolve every selected branch before building anything, so a typo or a
# missing branch cannot leave a half-deployed set of servers.
sel_branches=()
sel_servers=()
sel_refs=()
sel_commits=()
sel_subjects=()
missing=()
for i in "${!BRANCHES[@]}"; do
  branch="${BRANCHES[$i]}"
  server="${SERVERS[$i]}"
  branch_selected "$branch" || continue

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
  if (( SKIP_MISSING )); then
    echo "warning: skipping missing branch(es): ${missing[*]}" >&2
  else
    echo "error: missing branch(es): ${missing[*]}" >&2
    echo "       create/push them, or pass --skip-missing." >&2
    exit 1
  fi
fi
if (( ${#sel_branches[@]} == 0 )); then
  die "nothing to deploy"
fi

if (( DRY_RUN )); then
  echo "==> Dry run: no builds or uploads"
  for i in "${!sel_branches[@]}"; do
    printf '    %-12s [%-12s] %s  -> %s:%s/gamex86.dll\n' \
      "${sel_branches[$i]}" "$(profile_for "${sel_branches[$i]}")" \
      "${sel_commits[$i]}" \
      "$REMOTE_HOST" "$(dest_dir_for "${sel_servers[$i]}")"
  done
  exit 0
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v i686-w64-mingw32-g++ >/dev/null 2>&1 \
  || die "i686-w64-mingw32-g++ not found (install the mingw-w64 i686 toolchain)"
[[ -x "$PYTHON3" ]] || die "Python interpreter not found: $PYTHON3"
"$PYTHON3" -c 'import yaml' >/dev/null 2>&1 \
  || die "$PYTHON3 does not have PyYAML (set PYTHON3 to a suitable interpreter)"

if (( ! BUILD_ONLY )); then
  command -v ssh >/dev/null 2>&1 || die "ssh not found"
  command -v scp >/dev/null 2>&1 || die "scp not found"

  echo "==> Checking destination directories on $REMOTE_HOST..."
  for i in "${!sel_branches[@]}"; do
    dest_dir="$(dest_dir_for "${sel_servers[$i]}")"
    if ! ssh "$REMOTE_HOST" "test -d $(shell_quote "$dest_dir")"; then
      die "cannot access $REMOTE_HOST:$dest_dir (SSH failed or directory is missing)"
    fi
  done
fi

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

if (( BUILD_ONLY )); then
  echo
  echo "==> Build-only: skipping upload"
else
  echo
  echo "==> Uploading to $REMOTE_HOST..."
  for i in "${!sel_branches[@]}"; do
    branch="${sel_branches[$i]}"
    server="${sel_servers[$i]}"
    dest_dir="$(dest_dir_for "$server")"
    dest="$dest_dir/gamex86.dll"
    tmp="$dest_dir/.gamex86.dll.new.$$"

    echo "==> [$branch] $REMOTE_HOST:$dest"

    if (( BACKUP )); then
      ssh "$REMOTE_HOST" \
        "if [ -e $(shell_quote "$dest") ]; then cp -p $(shell_quote "$dest") $(shell_quote "$dest.bak.$STAMP"); fi"
    fi

    scp -q "${sel_dlls[$i]}" "$REMOTE_HOST:$tmp"
    ssh "$REMOTE_HOST" "mv -f $(shell_quote "$tmp") $(shell_quote "$dest")"

    remote_sha="$(ssh "$REMOTE_HOST" "sha256sum $(shell_quote "$dest")" | awk '{print $1}')"
    if [[ "$remote_sha" != "${sel_shas[$i]}" ]]; then
      die "[$branch] checksum mismatch after upload to $REMOTE_HOST:$dest"
    fi
  done

  if (( RESTART )); then
    echo
    echo "==> Restarting $RESTART_SERVICE (controls all servers)..."
    if ! ssh "$REMOTE_HOST" "sudo -n systemctl restart $(shell_quote "$RESTART_SERVICE")"; then
      die "failed to restart $RESTART_SERVICE (passwordless sudo required)"
    fi
    if ! ssh "$REMOTE_HOST" "systemctl is-active --quiet $(shell_quote "$RESTART_SERVICE")"; then
      die "$RESTART_SERVICE is not active after restart"
    fi
  fi
fi

echo
if (( BUILD_ONLY )); then
  echo "==> Built artifacts"
else
  echo "==> Installed artifacts on $REMOTE_HOST"
fi
for i in "${!sel_branches[@]}"; do
  printf '    %-12s [%-12s] -> %-11s %s  sha256 %.12s\n' \
    "${sel_branches[$i]}" "$(profile_for "${sel_branches[$i]}")" \
    "${sel_servers[$i]}" \
    "${sel_commits[$i]}" "${sel_shas[$i]}"
done

if (( ! BUILD_ONLY && ! RESTART )); then
  echo
  echo "Note: running servers keep the old DLLs until restarted (use --restart)."
fi
