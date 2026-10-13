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
# Every file was built by us and every link carries $ORIGIN, so the bundle is a
# copy: nothing missing, nothing that needs a launcher to find the rest.
grep -q 'bundle: .*, 0 unresolved, 0 on the launcher' "$WORK/export.log" || {
    echo "the bundle is not self-contained:"; cat "$WORK/export.log"; exit 1
}
[ -e "$WORK/bundle/run.sh" ] && { echo "a launcher was written anyway" >&2; exit 1; }
(cd / && "$WORK/bundle/pngprobe")

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
grep -q ' 0 unresolved, 0 on the launcher' "$WORK/two.log" ||
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

echo "=== a project: its own manifest, its own repositories, its own build ==="
# A manifest is a recipe for the package this directory is, and the project
# resolves against the repositories it names — checked here with the built-in
# repository switched off, so nothing reaches the network.
PROJ="$WORK/proj"
rm -rf "$PROJ"
mkdir -p "$PROJ"
cat >"$PROJ/retrolunar.toml" <<EOF
deps = ["zlib@$native"]

build = """
mkdir -p \"\$PREFIX/bin\"
printf '#!/bin/sh\\necho proj from \$PREFIX\\n' > \"\$PREFIX/bin/proj\"
chmod +x \"\$PREFIX/bin/proj\"
"""

[repo.packages]
url = "$PACKAGES"
EOF
PNEST="$WORK/proj-nest"
rm -rf "$PNEST"
( cd "$PROJ" && "$RL" --nest "$PNEST" --no-default-repo run sh -- -c 'echo "zlib at $zlib_PREFIX"' ) \
    >"$WORK/proj-run.log" 2>&1 ||
    { echo "the project's environment failed:"; tail -8 "$WORK/proj-run.log"; exit 1; }
grep -q "zlib at $PNEST/zlib@$native" "$WORK/proj-run.log" ||
    { echo "the environment does not name the nest:"; cat "$WORK/proj-run.log"; exit 1; }
echo "the manifest's dependencies resolved from the repository it names"

# `build --out DIR` installs this package there, dependencies staying in the nest.
rm -rf "$PROJ/build"
( cd "$PROJ" && "$RL" --nest "$PNEST" --no-default-repo build --out "$PROJ/build" ) \
    >"$WORK/proj-build.log" 2>&1 ||
    { echo "build --out failed:"; tail -8 "$WORK/proj-build.log"; exit 1; }
[ -x "$PROJ/build/bin/proj" ] ||
    { echo "build --out did not install the package:"; cat "$WORK/proj-build.log"; exit 1; }
case "$("$PROJ/build/bin/proj")" in
"proj from $PROJ/build") ;;
*) echo "the program built into --out does not name it:"; "$PROJ/build/bin/proj"; exit 1 ;;
esac
[ ! -e "$PROJ/build/zlib@$native" ] || { echo "--out holds a dependency:"; exit 1; }
echo "build --out: this package in DIR, its dependencies in the nest"

# The lock says what the build used, and entries are addressed by content.
grep -q '^\[entries\]' "$PROJ/retrolunar.lock" ||
    { echo "no lock was written:"; cat "$PROJ/retrolunar.lock" 2>/dev/null; exit 1; }
entries=$(grep -c '^"' "$PROJ/retrolunar.lock")
[ -d "$PNEST/store" ] || { echo "built packages are not in a store:"; exit 1; }
[ -L "$PNEST/zlib@$native" ] ||
    { echo "a name is not a link into the store: $(ls -ld "$PNEST/zlib@$native")"; exit 1; }
echo "the lock names $entries entries, and zlib@$native is a link into the store"

# Two versions of one package keep their own entry: the second build adds one
# instead of overwriting what the first left.
before=$(ls "$PNEST/store" | wc -l)
sed -i 's/echo proj from/echo proj again from/' "$PROJ/retrolunar.toml"
( cd "$PROJ" && "$RL" --nest "$PNEST" --no-default-repo build ) >"$WORK/proj-again.log" 2>&1 ||
    { echo "the second build failed:"; tail -8 "$WORK/proj-again.log"; exit 1; }
after=$(ls "$PNEST/store" | wc -l)
[ "$after" -gt "$before" ] ||
    { echo "a changed recipe overwrote its entry ($before -> $after)"; exit 1; }
case "$("$PNEST/proj@$native/bin/proj")" in
*"proj again from"*) ;;
*) echo "the name does not point at the new build:"; "$PNEST/proj@$native/bin/proj"; exit 1 ;;
esac
echo "a changed recipe is a second entry, and the name points at it"

# gc removes what nothing points at, and only that.
"$RL" --nest "$PNEST" gc >"$WORK/proj-gc.log" 2>&1 || { echo "gc failed:"; cat "$WORK/proj-gc.log"; exit 1; }
[ -x "$PNEST/proj@$native/bin/proj" ] || { echo "gc removed a package in use"; exit 1; }
grep -q 'in use' "$WORK/proj-gc.log" || { echo "gc said nothing useful:"; cat "$WORK/proj-gc.log"; exit 1; }
echo "gc: $(tail -1 "$WORK/proj-gc.log" | sed 's/^retrolunar: //')"

echo
echo "acceptance: ok (work in $WORK)"
