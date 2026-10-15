#!/bin/sh
# The acceptance run: everything that has to hold after a change.
#
#   tests/run.sh
#
# It builds the tool, builds the test chain through four build systems, and
# checks freshness, export and the freshness rules. It never runs the v1 Lua
# port and never reads the Lua tree: the tree it tests is the one it is pointed
# at, and packages whose dependencies are not there yet are skipped rather than
# reported. Overridable:
#
#   PACKAGES=../retrolunar2-packages   the tree it tests
#   WORK=$(mktemp -d)                  everything temporary
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
PACKAGES=${PACKAGES:-$root/../retrolunar2-packages}
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

echo "=== the native system has to match this host ==="
# The native system names llvm's tools, which this host has as GNU ones; and it
# carries no probes. Both are host facts, so the test states them here rather
# than pretending the system file is what a build on this machine uses.
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
        # A dependency the tree does not have yet is not a defect - the
        # resolution step reports those. A script that will not parse
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

echo "=== an entry names itself, and can be moved ==="
# What a package records about itself is what a consumer reads, so it has to be the
# directory the entry really lives in — and to survive the entry moving, it is written
# relative to the file that says it (pkg-config's ${pcfiledir}). A path baked at build
# time points at a staging tree that is gone, or at a nest that has moved away.
command -v pkg-config >/dev/null ||
    { echo "pkg-config is not here, so this check cannot run" >&2; exit 1; }
zpc=$NEST/zlib@$native/share/pkgconfig/zlib.pc
if ! grep -q 'pcfiledir' "$zpc"; then
    echo "zlib's .pc does not name itself: $(grep '^prefix=' "$zpc")" >&2
    exit 1
fi
if grep -q "$NEST" "$zpc"; then
    echo "zlib's .pc still records the path it was built at: $(grep '^prefix=' "$zpc")" >&2
    exit 1
fi
zentry=$(readlink -f "$NEST/zlib@$native")
lentry=$(readlink -f "$NEST/libpng@$native")
zprefix=$(PKG_CONFIG_LIBDIR="$zentry/share/pkgconfig" pkg-config --variable=prefix zlib)
[ -f "$zprefix/include/zlib.h" ] ||
    { echo "zlib's .pc points at nothing: $zprefix" >&2; exit 1; }
# The move is the proof: the same file, somewhere else, answering the same question.
mv "$NEST" "$WORK/moved"
moved=$(PKG_CONFIG_LIBDIR="$WORK/moved/store/$(basename "$zentry")/share/pkgconfig" \
        pkg-config --variable=prefix zlib)
[ -f "$moved/include/zlib.h" ] ||
    { echo "after the move, zlib's .pc points at nothing: $moved" >&2; exit 1; }
# And a program has to find its own entry's libraries afterwards, not the host's: a program
# is installed into bin/ and its libraries into lib/, so $ORIGIN alone is the bin directory,
# where the loader finds nothing and silently takes a library of the same soname from the
# host — the case that hid here was our libpng 1.6.48 replaced by the system's 1.6.58.
resolved=$(ldd "$WORK/moved/store/$(basename "$lentry")/bin/pngfix" 2>/dev/null |
           sed -n 's/.*libpng16\.so\.16 => \([^ ]*\).*/\1/p')
case "$resolved" in
    "$WORK/moved/store/"*) ;;
    *) echo "after the move, pngfix takes libpng16 from ${resolved:-nowhere}" >&2; exit 1 ;;
esac
mv "$WORK/moved" "$NEST"
echo "zlib's .pc answered $zprefix, and $moved after the whole nest moved away"
echo "and pngfix found $resolved"
# And the pass says what it could not relax: a path compiled into a binary stays.
grep -q 'still name the entry by path' "$WORK/build.log" ||
    { echo "the relax pass reported nothing it could not do" >&2; exit 1; }

echo "=== every ELF we ship finds its libraries by \$ORIGIN ==="
# One generated script builds several nodes in one process, and a flag spelled for one
# toolchain that reaches another is how `$ORIGIN` came out as `RIGIN` in a library: make
# read `$O` as one of its own variables. The runtime path of every ELF in every entry is
# the check, so the next time it happens the run stops here.
find "$NEST/store" -type f >"$WORK/elf-list"
n=0; bad=0
while read -r f; do
    readelf -d "$f" 2>/dev/null | grep -q 'Dynamic section' || continue
    n=$((n+1))
    rp=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*(\(RUNPATH\|RPATH\)).*\[\(.*\)\]/\2/p')
    case ":$rp:" in
        *':$ORIGIN:'*) ;;
        *) echo "  $(basename "$f"): no \$ORIGIN in the runtime path: $rp"; bad=$((bad+1)) ;;
    esac
    case ":$rp:" in
        *':RIGIN:'*) echo "  $(basename "$f"): a mangled \$ORIGIN: $rp"; bad=$((bad+1)) ;;
    esac
    case "$rp" in
        *"$NEST/tmp/"*) echo "  $(basename "$f"): a staging path in the runtime path: $rp"; bad=$((bad+1)) ;;
    esac
done <"$WORK/elf-list"
echo "checked $n ELF file(s), $bad bad"
[ "$bad" -eq 0 ] || exit 1

echo "=== the nest keeps its own scratch, and says when it is in RAM ==="
# Everything a build writes transiently goes under $NESTDIR/tmp and each node
# removes it when it ends, so a nest is self-contained and /tmp is never involved.
# The one thing the script cannot fix is the caller's choice of nest: a nest on a
# tmpfs is a nest in RAM, and a long build there dies with "Disk quota exceeded",
# which reads like a broken recipe. It says so instead.
if [ -n "$(ls -A "$NEST/tmp" 2>/dev/null)" ]; then
    echo "the nest's tmp/ is not empty after a build:" >&2
    ls -A "$NEST/tmp" >&2
    exit 1
fi
echo "the nest's tmp/ is empty: work and stage are removed per node"
if [ "$(stat -f -c %T /tmp 2>/dev/null)" = tmpfs ]; then
    rm -rf /tmp/retrolunar-probe
    "$RL" --packages "$PACKAGES" --nest /tmp/retrolunar-probe generate -x 'hello@source' \
        >/dev/null 2>"$WORK/tmpfs.log" ||
        { echo "the probe build in /tmp failed:"; cat "$WORK/tmpfs.log"; exit 1; }
    grep -q 'NESTDIR is on a tmpfs' "$WORK/tmpfs.log" ||
        { echo "a nest on a tmpfs did not say so:"; cat "$WORK/tmpfs.log"; exit 1; }
    rm -rf /tmp/retrolunar-probe
    echo "a nest on a tmpfs says so before the work starts"
else
    echo "this /tmp is not a tmpfs: the warning is not exercised here"
fi

echo "=== one nest, several builds ==="
# The lock is per node, not per nest: what builds share is the store, and an entry is
# named by its content, so two builds of different nodes are ordinary. A second build of
# the *same* node waits on that node's lock and then finds it fresh — the proof being
# that the shared dependency is compiled once across the two runs, not twice.
CNEST="$WORK/conc-nest"
rm -rf "$CNEST"
"$RL" --packages "$PACKAGES" --nest "$CNEST" generate -o "$WORK/conc.sh" "zlib@$native" \
    >"$WORK/conc-gen.log" 2>&1 ||
    { echo "could not generate the concurrency script:"; cat "$WORK/conc-gen.log"; exit 1; }
sh -n "$WORK/conc.sh" || { echo "the concurrency script does not parse"; exit 1; }
env -i PATH="$PATH" HOME="$HOME" sh "$WORK/conc.sh" >"$WORK/conc-a.log" 2>&1 &
first=$!
sleep 1
env -i PATH="$PATH" HOME="$HOME" sh "$WORK/conc.sh" >"$WORK/conc-b.log" 2>&1 || true
wait "$first" || { echo "the first of the two builds failed:"; tail -6 "$WORK/conc-a.log"; exit 1; }
built=$(cat "$WORK/conc-a.log" "$WORK/conc-b.log" | grep -c 'relax: 1')
[ "$built" = "1" ] ||
    { echo "the shared dependency was built $built times, not once"; exit 1; }
grep -q "skip zlib@$native (fresh)" "$WORK/conc-b.log" ||
    { echo "the second build did not skip what the first one built:"; cat "$WORK/conc-b.log"; exit 1; }
echo "two builds in one nest: the shared dependency was compiled once, the second skipped it"

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

echo "=== the cache: built here, used there without building ==="
CCACHE="$WORK/cache"
CNEST="$WORK/cache-nest"
rm -rf "$CCACHE" "$CNEST"
"$RL" cache push --packages "$PACKAGES" --nest "$NEST" --cache "$CCACHE" "m4@$native" \
    >"$WORK/cache-push.log" 2>&1 ||
    { echo "cache push failed:"; tail -6 "$WORK/cache-push.log"; exit 1; }
"$RL" cache pull --packages "$PACKAGES" --nest "$CNEST" --cache "$CCACHE" "m4@$native" \
    >"$WORK/cache-pull.log" 2>&1 ||
    { echo "cache pull failed:"; tail -6 "$WORK/cache-pull.log"; exit 1; }
[ -x "$CNEST/m4@$native/bin/m4" ] ||
    { echo "the pulled package did not arrive:"; cat "$WORK/cache-pull.log"; exit 1; }
case "$("$CNEST/m4@$native/bin/m4" --version | head -1)" in
*1.4.20*) ;;
*) echo "the pulled package does not run:"; "$CNEST/m4@$native/bin/m4" --version; exit 1 ;;
esac
echo "an entry built here ran from another nest: $("$CNEST/m4@$native/bin/m4" --version | head -1)"

# A cache is bytes from somewhere else: they have to be the bytes that were
# published, and an entry that is not is refused rather than unpacked.
CTAMPER="$WORK/cache-tamper"
f=$(ls "$CCACHE"/*.tar.gz | head -1)
printf 'x' >>"$f"
rm -rf "$CTAMPER"
# The pull is meant to fail: the entry it wanted was not usable, so it exits
# non-zero and says why. The check is the message, not the status.
"$RL" cache pull --packages "$PACKAGES" --nest "$CTAMPER" --cache "$CCACHE" "m4@$native" \
    >"$WORK/cache-tamper.log" 2>&1 || true
grep -q 'does not match its digest' "$WORK/cache-tamper.log" ||
    { echo "a tampered entry was not refused:"; cat "$WORK/cache-tamper.log"; exit 1; }
echo "a tampered entry was refused: $(tail -1 "$WORK/cache-tamper.log" | sed 's/^retrolunar: //')"

echo "=== a project: its own manifest, its own repositories, its own build ==="
# A manifest is a recipe for the package this directory is, and the project
# resolves against the repositories it names — checked here with the built-in
# repository switched off, so nothing reaches the network.
PROJ="$WORK/proj"
rm -rf "$PROJ"
mkdir -p "$PROJ"
# The manifest is written with a quoted heredoc: nothing in it is the shell's to
# interpret, so no backslash has to be counted. The two values that vary are
# placeholders, substituted once below.
cat >"$PROJ/retrolunar.toml" <<'EOF'
deps = ["zlib@NATIVE_SYS"]

build = """
mkdir -p "$PREFIX/bin"
printf '#!/bin/sh\necho proj from %s\n' "$OUTPKG" > "$PREFIX/bin/proj"
chmod +x "$PREFIX/bin/proj"
"""

[repo.packages]
url = "@PACKAGES@"
EOF
sed -i "s|NATIVE_SYS|$native|; s|@PACKAGES@|$PACKAGES|" "$PROJ/retrolunar.toml"
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

echo "=== the example the documentation walks through builds ==="
# The tutorial's transcripts come from examples/mytool, and an example that stops
# building is documentation that lies. It is copied out of the tree, pointed at this
# tree the way a reader points it at theirs, and built with the built-in repository
# left out: the two repositories the manifest names are all it gets.
EX="$WORK/mytool"
rm -rf "$EX" "$WORK/example-nest"
cp -r "$root/examples/mytool" "$EX"
rm -f "$EX/retrolunar.lock"
sed -i "s|url = \"../../../retrolunar2-packages\"|url = \"$PACKAGES\"|" "$EX/retrolunar.toml"
( cd "$EX" && "$RL" --nest "$WORK/example-nest" --no-default-repo build ) >"$WORK/example.log" 2>&1 ||
    { echo "the documented example does not build:"; tail -8 "$WORK/example.log"; exit 1; }
got=$("$WORK/example-nest/mytool@$native/bin/mytool")
[ "$got" = "the answer is 42, from mylib" ] ||
    { echo "the example's program answered '$got'"; exit 1; }
( cd "$EX" && "$RL" --nest "$WORK/example-nest" --no-default-repo build ) >"$WORK/example2.log" 2>&1 ||
    { echo "the example's second build failed:"; tail -8 "$WORK/example2.log"; exit 1; }
grep -q "skip mytool@$native (fresh)" "$WORK/example2.log" ||
    { echo "the example rebuilt although nothing changed:"; cat "$WORK/example2.log"; exit 1; }
echo "the example builds, its program answers '$got', and a second build is fresh"

echo
echo "acceptance: ok (work in $WORK)"
