#!/bin/sh
# What makes a package stale, and what does not.
#
#   tests/hashes.sh PACKAGES NEST TARGET
#
# A change anywhere in a system's directory — any file, TOML or not — makes every
# package of that system stale, while the fetch-only nodes stay fresh. An unused
# file in a package's own directory changes nothing. A change to the build file
# that wins rebuilds that package and everything that depends on it. And taking
# the change away brings the tree back to fully fresh, which is what proves the
# digest is of the content and nothing else.
set -eu

PACKAGES=$1
NEST=$2
TARGET=$3
RL=${RL:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/builddir/retrolunar}
LOG=${LOG:-$(mktemp "${TMPDIR:-/tmp}/retrolunar-hashes-XXXXXX.log")}
TOTAL=$("$RL" deps --packages "$PACKAGES" --nest "$NEST" "$TARGET" | wc -l)

run() {
    "$RL" generate --packages "$PACKAGES" --nest "$NEST" -x "$TARGET" >"$LOG" 2>&1 ||
        { echo "  RUN FAILED: $(tail -2 "$LOG")"; return 0; }
    printf '  skipped %s of %s nodes\n' "$(grep -c '(fresh)$' "$LOG")" "$TOTAL"
}

native=${TARGET##*@}
echo "=== freshness (chain of $TOTAL nodes) ==="
echo "a) as it stands"; run
echo "b) a new file in the system's own directory"
echo "note" >"$PACKAGES/system@$native/not-a-toml-file.txt"; run
echo "c) taken away again"; rm -f "$PACKAGES/system@$native/not-a-toml-file.txt"; run; run
echo "d) a file the recipe does not use, inside the package directory"
echo "note" >"$PACKAGES/libpng/not-used.md"; run; rm -f "$PACKAGES/libpng/not-used.md"
echo "e) the build file that wins changes"
cp "$PACKAGES/libpng/generic.toml" "$LOG.bak"
printf '\n# a comment is a change\n' >>"$PACKAGES/libpng/generic.toml"; run
cp "$LOG.bak" "$PACKAGES/libpng/generic.toml"; run
echo "f) a state file written by another tool"
# The tool's version is an input of every fingerprint, and a state file says which
# tool wrote it. A shortcut that trusted such a file would hand the script the stored
# value to compare against itself, so an upgraded tool rebuilt nothing: the fingerprint
# here is one no tool could compute, and the state is the newest file in the nest.
state="$NEST/.retrolunar/state/$native/libpng.toml"
[ -f "$state" ] || { echo "  no state at $state"; exit 1; }
cp "$state" "$LOG.state.bak"
sed -e 's/^tool = .*/tool = "0.0.0"/' \
    -e 's/^fingerprint = .*/fingerprint = "0000000000000000000000000000000000000000000000000000000000000000"/' \
    "$state" >"$state.new"
mv "$state.new" "$state"
run
if grep -q "skip libpng@$native (fresh)" "$LOG"; then
    echo "  libpng stayed fresh with a state file written by another tool"
    exit 1
fi
grep -q "^tool = \"$("$RL" --version | awk '{print $2}')\"$" "$state" ||
    { echo "  the state was not rewritten by this tool:"; cat "$state"; exit 1; }
cp "$LOG.state.bak" "$state"
echo "g) and fresh again"; run
echo "log: $LOG"
