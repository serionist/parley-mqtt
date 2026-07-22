#!/usr/bin/env bash
#
# bump-version.sh — set (or verify) the Parley release version across all four ecosystems.
#
# Parley ships one lockstep version. It lives in FIVE manifest fields:
#   1. csharp/Directory.Build.props   <Version>X.Y.Z</Version>
#   2. js/package.json                "version": "X.Y.Z"
#   3. python/pyproject.toml          version = "X.Y.Z"        (the [project] version)
#   4. cpp/library.properties         version=X.Y.Z
#   5. cpp/library.json               "version": "X.Y.Z"       (the top-level version)
#
# Usage:
#   scripts/bump-version.sh              # check: print all five, exit non-zero if they disagree
#   scripts/bump-version.sh --check      # same as above
#   scripts/bump-version.sh X.Y.Z        # set all five to X.Y.Z, then re-check
#
# The CI release workflow re-verifies that the pushed tag (vX.Y.Z) equals all five fields, so a
# drift here fails the release rather than shipping mismatched packages.
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CSPROJ_PROPS="$ROOT/csharp/Directory.Build.props"
PKG_JSON="$ROOT/js/package.json"
PYPROJECT="$ROOT/python/pyproject.toml"
LIB_PROPS="$ROOT/cpp/library.properties"
LIB_JSON="$ROOT/cpp/library.json"

# --- extraction (first/only occurrence of each field) ---------------------------------------
get_csharp() { sed -nE 's@.*<Version>([^<]+)</Version>.*@\1@p' "$CSPROJ_PROPS" | head -1; }
get_js()     { sed -nE 's/^[[:space:]]*"version":[[:space:]]*"([^"]+)".*/\1/p' "$PKG_JSON" | head -1; }
get_py()     { sed -nE 's/^version[[:space:]]*=[[:space:]]*"([^"]+)".*/\1/p' "$PYPROJECT" | head -1; }
get_cprops() { sed -nE 's/^version=(.+)/\1/p' "$LIB_PROPS" | head -1; }
get_cjson()  { sed -nE 's/^[[:space:]]*"version":[[:space:]]*"([^"]+)".*/\1/p' "$LIB_JSON" | head -1; }

field_labels=(
  "csharp/Directory.Build.props"
  "js/package.json"
  "python/pyproject.toml"
  "cpp/library.properties"
  "cpp/library.json"
)

read_all() {
  VALUES=(
    "$(get_csharp)"
    "$(get_js)"
    "$(get_py)"
    "$(get_cprops)"
    "$(get_cjson)"
  )
}

print_all() {
  read_all
  local i
  for i in "${!field_labels[@]}"; do
    printf '  %-32s %s\n' "${field_labels[$i]}" "${VALUES[$i]:-<none>}"
  done
}

check() {
  read_all
  local v0="${VALUES[0]}" i ok=1
  for i in "${!VALUES[@]}"; do
    if [[ -z "${VALUES[$i]}" ]]; then
      echo "ERROR: could not read version from ${field_labels[$i]}" >&2
      ok=0
    elif [[ "${VALUES[$i]}" != "$v0" ]]; then
      ok=0
    fi
  done
  print_all
  if [[ "$ok" -ne 1 ]]; then
    echo "INCONSISTENT: the five version fields do not all match." >&2
    return 1
  fi
  echo "OK: all five manifests are at $v0"
  return 0
}

# --- setting (each edits exactly its one field) ---------------------------------------------
# JSON files can contain more than one "version" key (library.json also pins ArduinoJson), so the
# JSON setter rewrites only the FIRST matching line, which is always the top-level package version.
set_json_first_version() {
  local file="$1" new="$2" tmp
  tmp="$(mktemp)"
  awk -v new="$new" '
    !done && /"version"[[:space:]]*:/ {
      sub(/"version"[[:space:]]*:[[:space:]]*"[^"]*"/, "\"version\": \"" new "\"")
      done = 1
    }
    { print }
  ' "$file" > "$tmp"
  mv "$tmp" "$file"
}

set_version() {
  local new="$1"
  sed -i -E "s@<Version>[^<]*</Version>@<Version>${new}</Version>@" "$CSPROJ_PROPS"
  set_json_first_version "$PKG_JSON" "$new"
  sed -i -E "0,/^version[[:space:]]*=[[:space:]]*\"[^\"]*\"/ s//version = \"${new}\"/" "$PYPROJECT"
  sed -i -E "s@^version=.*@version=${new}@" "$LIB_PROPS"
  set_json_first_version "$LIB_JSON" "$new"
}

# --- main -----------------------------------------------------------------------------------
arg="${1:---check}"

case "$arg" in
  --check|-c)
    echo "Parley version fields:"
    check
    ;;
  -h|--help)
    grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
    ;;
  *)
    if [[ ! "$arg" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
      echo "ERROR: '$arg' is not a valid version (expected X.Y.Z or X.Y.Z-prerelease)." >&2
      exit 2
    fi
    echo "Setting all five manifests to $arg ..."
    set_version "$arg"
    check
    echo
    echo "Next: commit the change, then tag v$arg to trigger the release workflow."
    ;;
esac
