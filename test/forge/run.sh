#!/usr/bin/env bash
# Integration test for the file-API strategy: run the built nix-ci-config
# against a stand-in forge on loopback, for each forge the tool knows.
#
# This is the strategy the tool exists for, and the only one the other
# integration test cannot reach, because exercising it means answering a
# forge's contents API. A local server answers it instead, from the same
# fixture repositories, so the whole path is covered with no network: the URLs,
# the auth header, the base64, the file and directory handling, and the
# evaluation through the accessor.
#
# The number of requests is asserted as well as the output, because the claim
# the fast path makes is not just that it answers but that it answers cheaply,
# and nothing else observes that.
#
# Set UPDATE_GOLDEN=1 to rewrite the golden from the current output.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_CONFIG_BIN:-$here/../../builddir/nix-ci-config}"
fixtures="$here/../integration/fixtures"
golden="$here/expected.txt"

# The revision the stand-in forge is pinned at. Any other is a 404, so a tool
# that dropped the revision from its requests would fail rather than pass.
rev=1111111111111111111111111111111111111111

work="$(mktemp -d)"
server_pid=""
cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  chmod -R u+w "$work" 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

# The request count, which is what the fast path claims, is asserted from the
# stand-in forge's own log (the "asked" lines below), not from the tool's
# output, so it is not needed here. A fetch-error message carries the port the
# forge got, which is whatever was free, so it is compared by its result alone.
normalise='
  if .result == "success" then
    "success " + (.config | tojson)
  elif .result == "not-self-contained" then
    "not-self-contained " + .path
  else
    .result
  end'

actual=""
for forge in github gitlab forgejo; do
  # flake-output and no-config have no configuration file, so the configuration
  # is the flake's `nix-ci` output; both are read from flake.nix alone.
  # flake-output-input-pattern is the case that matters most: its `outputs`
  # destructures `self` and an input, as almost every real flake does, but its
  # `nix-ci` output uses neither, so it too is read from flake.nix alone and the
  # input is never fetched. The flake-output-uses-self case is the opposite: its
  # output looks at `self`, so it cannot be read that way and the fast path
  # defers to a full fetch. That fetch is a real clone the stand-in forge cannot
  # answer, so here it surfaces as an error rather than the configuration a real
  # forge would return; what the case pins is that the fast path did not answer
  # such an output from flake.nix, having asked only for the two files first.
  for fixture in config config-import config-nested flake-output flake-output-input-pattern no-config flake-output-uses-self no-flake unreadable; do
    log="$work/requests-$forge-$fixture.txt"
    : > "$log"

    # A repository the credentials cannot see answers 404 for every path,
    # exactly as one with no flake.nix does for that one file. Standing in for
    # it by asking for a revision the server does not have makes every request
    # answer the same way a forge would for a repository it will not show us.
    # The two mean opposite things, so reporting the wrong one fails a job that
    # should have been retried, and tells the user to write a flake they have.
    asked="$rev"
    served="$fixtures/$fixture"
    if [ "$fixture" = unreadable ]; then
      served="$fixtures/config"
      asked=2222222222222222222222222222222222222222
    fi

    python3 "$here/server.py" "$served" 0 "$log" "$rev" > "$work/port.txt" &
    server_pid=$!
    # The server prints the port it got once it is listening.
    for _ in $(seq 1 100); do
      port="$(cat "$work/port.txt" 2>/dev/null || true)"
      [ -n "$port" ] && break
      sleep 0.1
    done
    if [ -z "${port:-}" ]; then
      echo "the stand-in forge did not start" >&2
      exit 1
    fi

    set +e
    output="$(
      env \
        NIX_STORE_DIR="$work/store" \
        NIX_STATE_DIR="$work/state" \
        NIX_LOG_DIR="$work/log" \
        NIX_DATA_DIR="$work/data" \
        HOME="$work/home" \
        "$bin" \
          --flake "git+http://127.0.0.1:$port/owner/repo?rev=$asked" \
          --forge-type "$forge" 2>/dev/null \
        | jq -r "$normalise"
    )"
    status=$?
    set -e

    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=""

    actual+="$forge $fixture exit=$status"$'\n'
    if [ -n "$output" ]; then
      actual+="$(printf '%s\n' "$output" | sed 's/^/  /')"$'\n'
    fi
    actual+="$(sed 's/^/  asked /' "$log")"$'\n'
  done
done

if [ "${UPDATE_GOLDEN:-}" = 1 ]; then
  printf '%s' "$actual" > "$golden"
  echo "updated $golden"
  exit 0
fi

if ! diff -u "$golden" <(printf '%s' "$actual"); then
  echo "nix-ci-config forge-api output does not match the golden." >&2
  echo "Re-run with UPDATE_GOLDEN=1 to accept the new output." >&2
  exit 1
fi

echo "nix-ci-config forge-api test passed"
