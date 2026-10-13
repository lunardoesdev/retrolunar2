#!/bin/sh
# The acceptance run: everything that has to hold after a change.
#
#   tests/run.sh
#
# It regenerates the ported tree from the v1 sources when they are present,
# builds the tool, builds the test chain through four build systems, and checks
# freshness, export and the freshness rules. Overridable:
#
#   PACKAGES=../retrolunar2-packages   the ported tree it installs into
#   SOURCES=../retrolunar-packages     v1 sources, when porting
#   WORK=$(mktemp -d)                  everything temporary
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
PACKAGES=${PACKAGES:-$root/../retrolunar2-packages}
SOURCES=${SOURCES:-$root/../retrolunar-packages}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/retrolunar-test-XXXXXX")}
NEST=$WORK/nest
CHAIN='pngprobe@clang-native'

[ -d "$PACKAGES" ] || { echo "no packages tree at $PACKAGES" >&2; exit 1; }
mkdir -p "$WORK"

echo "=== tool ==="
meson setup "$root/builddir" "$root" >/dev/null 2>&1 || true
ninja -C "$root/builddir" >/dev/null
RL=$root/builddir/retrolunar
"$RL" --help >/dev/null
echo "built $RL"

if [ -d "$SOURCES" ]; then
    echo "=== port from $SOURCES ==="
    python3 "$root/tools/lua2toml.py" packages "$SOURCES" "$WORK/pkg" \
        --report "$WORK/pkg/PORTING.md" 2>&1 | tail -1
    python3 "$root/tools/lua2toml.py" systems "$SOURCES" "$WORK/sys" \
        --report "$WORK/sys/PORTING.md" --uses "$WORK/pkg/.port-uses.json" 2>&1 | tail -1
    rm -rf "$PACKAGES"/system@*
    cp -rf "$WORK"/sys/system@* "$PACKAGES/"
    for d in "$WORK"/pkg/*/; do
        n=$(basename "$d")
        rm -rf "$PACKAGES/$n"
        cp -rf "$d" "$PACKAGES/$n"
    done
    cp "$WORK/pkg/PORTING.md" "$PACKAGES/PORTING.md" 2>/dev/null || true
    cp "$WORK/sys/PORTING.md" "$PACKAGES/PORTING-systems.md" 2>/dev/null || true
    echo "installed $(ls -d "$PACKAGES"/system@* | wc -l) systems and $(ls "$PACKAGES" | grep -vc 'system@\|PORTING\|README\|^\.') packages"
fi

echo "=== the native system has to match this host ==="
# The v1 native system names llvm's tools, which this host has as GNU ones; and
# it carries no probes. Both are host facts, so the test states them here rather
# than pretending the ported file is what a build on this machine uses.
# Ask the tool what a bare target resolves to: that is the native system, and it
# checks the default at the same time.
native=$("$RL" deps --packages "$PACKAGES" zlib | tail -1 | sed 's/.*@//')
[ -n "$native" ] || { echo "no native system found" >&2; exit 1; }
python3 - "$PACKAGES/system@$native/system.toml" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
for var, val in (("AR", "ar"), ("RANLIB", "ranlib"), ("LD", "ld"), ("STRIP", "strip"),
                 ("OBJCOPY", "objcopy"), ("READELF", "readelf"), ("OBJDUMP", "objdump")):
    s = re.sub(r'(?m)^%s = ".*"$' % var, '%s = "%s"' % (var, val), s)
s = re.sub(r'(?m)^probe = .*\n', '', s)
s = s.replace('MAKEFLAGS = "-j$CORES"',
              'MAKEFLAGS = "-j$CORES"\nprobe = ["clang --version", "meson --version", '
              '"cmake --version", "pkg-config --version"]')
open(p, "w").write(s)
PY
echo "adapted system@$native"

echo "=== every package emits a script that parses ==="
bad=0; n=0; skipped=0
for d in "$PACKAGES"/*/; do
    pkg=$(basename "$d")
    case "$pkg" in system@*) continue;; esac
    n=$((n+1))
    if ! "$RL" generate --packages "$PACKAGES" --nest "$WORK/parse" -o "$WORK/parse.sh" "$pkg@$native" >"$WORK/parse.err" 2>&1; then
        # A dependency the tree does not have yet is the port's queue, not a
        # defect: the resolution step reports those. A script that will not parse
        # is a defect, because one of them stops every target beside it.
        if grep -qiE "no package|no recipe for system|no system" "$WORK/parse.err"; then
            skipped=$((skipped+1))
        else
            echo "  $pkg: $(head -1 "$WORK/parse.err")"; bad=$((bad+1))
        fi
        continue
    fi
    sh -n "$WORK/parse.sh" 2>"$WORK/parse.err" || { echo "  $pkg: does not parse"; head -1 "$WORK/parse.err"; bad=$((bad+1)); }
done
echo "checked $n scripts, $bad bad, $skipped without one (missing dependencies)"
[ "$bad" -eq 0 ] || exit 1

echo "=== every package resolves on three systems ==="
fail=0; n=0
for p in $(ls "$PACKAGES" | grep -v 'system@\|PORTING\|README\|^\.'); do
    for sy in "$native" mingw64 aarch64-android24; do
        n=$((n + 1))
        if ! "$RL" deps --packages "$PACKAGES" "$p@$sy" >/dev/null 2>"$WORK/deps.err"; then
            msg=$(head -1 "$WORK/deps.err")
            case "$msg" in
                *"no package"*|*"no recipe"*) ;;
                *) fail=$((fail + 1)); [ "$fail" -le 6 ] && echo "  $p@$sy: $msg" ;;
            esac
        fi
    done
done
echo "checked $n combinations, real failures: $fail"
[ "$fail" -eq 0 ] || exit 1

echo "=== a clean build, through four build systems ==="
rm -rf "$NEST"
start=$(date +%s)
"$RL" generate --packages "$PACKAGES" --nest "$NEST" -x "$CHAIN" >"$WORK/build.log" 2>&1 ||
    { echo "BUILD FAILED"; grep -nE 'error|unbound|No such|not found' "$WORK/build.log" | tail -12; exit 1; }
echo "clean build: $(( $(date +%s) - start ))s"
start=$(date +%s)
"$RL" generate --packages "$PACKAGES" --nest "$NEST" -x "$CHAIN" >"$WORK/again.log" 2>&1 ||
    { echo "SECOND RUN FAILED"; tail -12 "$WORK/again.log"; exit 1; }
echo "rerun: $(( $(date +%s) - start ))s, skipped $(grep -c '(fresh)' "$WORK/again.log") nodes"
grep -q 'skip .* (fresh)' "$WORK/again.log" || { echo "nothing was skipped: freshness is broken" >&2; exit 1; }

echo "=== export, and the program it produces ==="
"$RL" env --packages "$PACKAGES" --nest "$NEST" "$CHAIN" >"$WORK/env.sh"
libs=$(ls -d "$NEST"/*@"$native"/lib "$NEST"/*@"$native"/lib64 2>/dev/null | tr '\n' ':' || true)
LD_LIBRARY_PATH=$libs "$NEST/pngprobe@$native/bin/pngprobe"
rm -rf "$WORK/bundle"
"$RL" export --packages "$PACKAGES" --nest "$NEST" --to "$WORK/bundle" "$CHAIN" >"$WORK/export.log" 2>&1 || {
    echo "export refused the bundle:"; tail -5 "$WORK/export.log"; exit 1
}
tail -2 "$WORK/export.log"
# Nothing the bundle needs may be missing, whatever else happened.
grep -q 'unresolved' "$WORK/export.log" && grep -q ' 0 unresolved' "$WORK/export.log" || {
    echo "the bundle misses a library:"; cat "$WORK/export.log"; exit 1
}
# Where the runtime path could be baked in, the file is relocatable and needs no
# help. Where an autotools link re-evaluated the value (make, then libtool), the
# bundle carries a launcher instead — say how many, so the number is visible.
if [ -e "$WORK/bundle/run.sh" ]; then
    grep -o 'no runtime path could be baked into [0-9]* file(s)' "$WORK/export.log" | head -1
    (cd / && "$WORK/bundle/run.sh" "$WORK/bundle/pngprobe")
else
    (cd / && "$WORK/bundle/pngprobe")
fi

# m4 is the tree's autotools package, and the tool every configure step would
# use: build it, then prove the binary runs — not just that the step returned.
"$RL" generate --packages "$PACKAGES" --nest "$NEST" -x "m4@$native" >"$WORK/m4.log" 2>&1 || {
    echo "m4@$native: the build failed" >&2
    tail -8 "$WORK/m4.log" >&2
    exit 1
}
if "$NEST/m4@$native/bin/m4" --version >"$WORK/m4.version" 2>&1; then
    echo "m4@$native: $("$NEST/m4@$native/bin/m4" --version | head -1)"
else
    echo "m4@$native: the binary does not run" >&2
    cat "$WORK/m4.version" >&2
    exit 1
fi

rm -rf "$WORK/two"
"$RL" export --packages "$PACKAGES" --nest "$NEST" --to "$WORK/two" "m4@$native" "pngprobe@$native" >"$WORK/two.log" 2>&1 || {
    echo "export of two targets failed:"; tail -5 "$WORK/two.log"; exit 1
}
for f in m4 pngprobe; do
    [ -x "$WORK/two/$f" ] || { echo "two-target bundle is missing $f:"; cat "$WORK/two.log"; exit 1; }
done
grep -q ' 0 unresolved' "$WORK/two.log" ||
    { echo "two-target bundle is incomplete:"; cat "$WORK/two.log"; exit 1; }
(cd / && "$WORK/two/m4" --version | head -1)

echo "=== the fifth build system, on a package that is only files to place ==="
"$RL" generate --packages "$PACKAGES" --nest "$NEST" -x "meson@$native" >"$WORK/python.log" 2>&1 ||
    { echo "PYTHON DRIVER FAILED"; tail -8 "$WORK/python.log"; exit 1; }
site=$("$RL" env --packages "$PACKAGES" --nest "$NEST" "meson@$native" | sed -n 's/^PYTHON_SITE=\"\(.*\)\"/\1/p' | head -1)
site=${site:-lib/python3.14/site-packages}
if [ -d "$NEST/meson@$native/$site/mesonbuild" ] && [ -x "$NEST/meson@$native/bin/meson" ]; then
    echo "meson@$native: module in $site, launcher in bin/"
else
    echo "meson@$native: expected $site/mesonbuild and bin/meson" >&2; exit 1
fi

sh "$here/hashes.sh" "$PACKAGES" "$NEST" "$CHAIN"

echo
echo "acceptance: ok (work in $WORK)"
