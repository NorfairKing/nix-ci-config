#!/usr/bin/env bash
# Integration test: run the built nix-ci-config against each hermetic fixture
# repository and compare its output to a golden.
#
# The fixtures are read through Nix's own accessor (a `path:` reference names
# no forge), so this covers the strategy that has to work everywhere and needs
# no network. What the forge file API builds and parses is unit-tested instead,
# in test/pure_test.cc, since exercising it would mean talking to a forge.
#
# The tool opens a Nix store, which must not be the caller's real one, so it is
# pointed at a throwaway one under a temp dir.
#
# An evaluation error carries file paths and Nix's exact formatting, neither of
# which is this tool's contract, so an error line is compared by its type
# alone. Everything else is compared exactly, because the configuration value
# and the line types are precisely what a consumer reads.
#
# Set UPDATE_GOLDEN=1 to rewrite the golden from the current output.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_CONFIG_BIN:-$here/../../builddir/nix-ci-config}"
golden="$here/expected.txt"

store="$(mktemp -d)"
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

# One JSON object per run, discriminated by "result". An error message carries
# file paths and Nix's exact formatting, neither of which is this tool's
# contract, so an eval-error is compared by its result alone; everything else
# is compared exactly, because the configuration value and the paths are what
# a consumer reads.
normalise='
  if .result == "success" then
    "success " + (.config | tojson)
  elif .result == "not-self-contained" then
    "not-self-contained " + .path
  elif .result == "eval-error" then
    .result
  else
    .result
  end'

actual=""
# Sorted in one collation whatever the caller's, or the golden would depend on
# the locale the test happens to run under.
for fixture in $(printf '%s\n' "$here"/fixtures/*/ | LC_ALL=C sort); do
  name="$(basename "$fixture")"
  set +e
  output="$(
    env \
      NIX_STORE_DIR="$store/store" \
      NIX_STATE_DIR="$store/state" \
      NIX_LOG_DIR="$store/log" \
      NIX_DATA_DIR="$store/data" \
      HOME="$store/home" \
      "$bin" --flake "path:$fixture" 2>/dev/null \
      | jq -r "$normalise"
  )"
  status=$?
  set -e
  actual+="$name exit=$status"$'\n'
  if [ -n "$output" ]; then
    actual+="$(printf '%s\n' "$output" | sed 's/^/  /')"$'\n'
  fi
done

if [ "${UPDATE_GOLDEN:-}" = 1 ]; then
  printf '%s' "$actual" > "$golden"
  echo "updated $golden"
  exit 0
fi

if ! diff -u "$golden" <(printf '%s' "$actual"); then
  echo "nix-ci-config integration output does not match the golden." >&2
  echo "Re-run with UPDATE_GOLDEN=1 to accept the new output." >&2
  exit 1
fi

echo "nix-ci-config integration test passed"
