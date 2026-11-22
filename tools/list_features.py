#!/usr/bin/env python3
"""Print each build profile and the features/descriptions it contains."""

import sys
import textwrap
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FEATURES_DIR = ROOT / "src" / "features"
FEATURES_YAML = FEATURES_DIR / "features.yaml"
SUMMARY_FILENAME = "SUMMARY.md"

sys.path.insert(0, str(TOOLS))
import generate_hooks as gh  # noqa: E402

PROFILE_BRANCH = {
    "stable": "master",
    "unstable": "unstable",
    "experimental": "experimental",
}
WRAP_WIDTH = 100


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_summary(name):
    path = FEATURES_DIR / name / SUMMARY_FILENAME
    if not path.is_file():
        fail(f"feature '{name}' is missing src/features/{name}/{SUMMARY_FILENAME}")
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as e:
        fail(f"cannot read {path}: {e}")

    # Use the first paragraph after an optional Markdown title. Further
    # sections are for humans and are not part of the one-line summary.
    while lines and not lines[0].strip():
        lines.pop(0)
    if lines and lines[0].lstrip().startswith("#"):
        lines.pop(0)
    while lines and not lines[0].strip():
        lines.pop(0)

    paragraph = []
    for line in lines:
        if not line.strip():
            break
        paragraph.append(line.strip())
    summary = " ".join(paragraph).strip()
    if not summary:
        fail(
            f"src/features/{name}/{SUMMARY_FILENAME} needs a description "
            "paragraph after the heading")
    return summary


def load_metadata():
    try:
        data = yaml.safe_load(FEATURES_YAML.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as e:
        fail(f"cannot read {FEATURES_YAML}: {e}")

    if not isinstance(data, dict):
        fail(f"{FEATURES_YAML}: expected a mapping")

    try:
        profiles = gh.parse_profiles(data)
    except ValueError as e:
        fail(str(e))

    features = data.get("features")
    if not isinstance(features, dict):
        fail(f"{FEATURES_YAML}: `features:` must be a mapping")

    known_dirs = {
        path.name for path in FEATURES_DIR.iterdir()
        if path.is_dir() and not path.name.startswith(".")
    }
    known_tiers = set().union(*profiles.values())
    metadata = {}
    for name, cfg in features.items():
        name = str(name)
        if not isinstance(cfg, dict):
            fail(f"feature '{name}' needs a mapping with `tier:`")
        tier = str(cfg.get("tier", "")).strip().lower()
        if not tier:
            fail(f"feature '{name}' has no `tier:`")
        if tier != "dev" and tier not in known_tiers:
            fail(f"feature '{name}' has unknown tier '{tier}'")
        metadata[name] = {"tier": tier, "description": read_summary(name)}

    missing = sorted(known_dirs - set(metadata))
    orphan = sorted(set(metadata) - known_dirs)
    if missing:
        fail("feature directories missing from features.yaml: " + ", ".join(missing))
    if orphan:
        fail("features.yaml entries without a feature directory: " + ", ".join(orphan))

    return data, profiles, metadata


def enabled_for(profile):
    try:
        return gh.parse_features_yaml(FEATURES_YAML, profile)
    except ValueError as e:
        fail(str(e))


def print_feature(name, metadata):
    item = metadata[name]
    print(f"  {name} [{item['tier']}]")
    print(textwrap.fill(
        item["description"],
        width=WRAP_WIDTH,
        initial_indent="    ",
        subsequent_indent="    ",
    ))


def main():
    if any(arg in ("-h", "--help") for arg in sys.argv[1:]):
        print("Usage: scripts/list-features.sh")
        print("Lists each build profile and the features it contains.")
        return
    if len(sys.argv) > 1:
        fail(f"unknown argument: {sys.argv[1]}")

    data, profiles, metadata = load_metadata()
    order = [str(name) for name in data["features"]]
    enabled = {
        profile: enabled_for(profile)
        for profile in gh.PROFILE_ORDER
    }
    for profile, names in enabled.items():
        unknown = sorted(set(names) - set(metadata))
        if unknown:
            fail(f"{profile} profile enables unknown feature(s): " + ", ".join(unknown))

    print("Feature sets by build")
    print(f"Tiers: {FEATURES_YAML.relative_to(ROOT)}")
    print("Descriptions: src/features/<name>/SUMMARY.md")
    print()

    previous = set()
    for profile in gh.PROFILE_ORDER:
        branch = PROFILE_BRANCH[profile]
        names = enabled[profile]
        added = [
            name for name in order
            if name in names and name not in previous
        ]
        if profile == gh.PROFILE_ORDER[0]:
            print(f"== {branch} ({profile} profile) - {len(names)} feature(s) ==")
        else:
            print(
                f"== {branch} ({profile} profile) - adds {len(added)} "
                f"feature(s), {len(names)} total ==")
        if not added:
            print("  (none)")
        for name in added:
            print_feature(name, metadata)
        previous.update(names)
        print()

    all_enabled = set().union(*enabled.values())
    not_built = [name for name in order if name not in all_enabled]
    if not_built:
        print("== not in any build (dev/disabled) ==")
        for name in not_built:
            print_feature(name, metadata)


if __name__ == "__main__":
    main()
