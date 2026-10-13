#!/usr/bin/env python3
"""Port v1 Lua recipes and systems to the v2 TOML format.

A migration aid, not part of the tool: it runs once over retrolunar-packages and writes
the TOML tree. It is kept in the repository because it documents exactly what the port
changed, and its report (`--report`) is the list of every line it dropped, rewrote or
flagged — a mechanically ported recipe that silently changed behaviour would be worse
than no port at all.

What it does, per file:

* `source.lua` → `source.toml`, `generic.lua` → `generic.toml`, `<sys>.lua` → `<sys>.toml`,
  a system's `generic.lua` → `system@<name>/` with `system.toml` and the buildsystem
  fragments split out of its `setup` fragment.
* `require("x")` becomes `deps = [...]`, keeping order and `@system` suffixes.
* `$OUT` (v1's staging directory) becomes `$PREFIX` (v2's staging path, which is
  `$DESTDIR$OUTPKG`), except where it names an install prefix for a build system — there it
  becomes `$OUTPKG`, the package's final directory, because that is the path config files
  must record and `DESTDIR` redirects the write.
* `$NESTDIR/source/<name>` becomes `$NESTDIR/<name>@source` (a source package now has its
  own directory like everything else).
* The system's `$PREFIX` search-path lines are dropped: the emitter builds those from the
  dependency closure now, per package. Anything the tool cannot map is flagged in place,
  and `$PREFIX` in a package body — where it meant "where my dependencies are" — becomes an
  undefined `${RETROLUNAR_PORT_PREFIX}` so the build stops instead of silently installing
  into the wrong place.
"""

import argparse
import json
import shlex
import os
import re
import shutil
import sys

# Sections v1 systems were written in, and where each belongs in v2.
BUILDSYSTEM_BY_PREFIX = [
    ("AUTOCONF_", "autoconf"),
    ("CMAKE_", "cmake"),
    ("MESON_", "meson"),
    ("CARGO_", "cargo"),
    ("RUSTFLAGS", "cargo"),
    ("PKG_CONFIG_ALLOW_CROSS", "cargo"),
]

# mingw64/mingw32 are what those toolchains are called, so the directory says
# which one it is. The native system keeps its name: `@native` is a target alias
# the CLI resolves, and a system called `native` would make that ambiguous.
RENAMES = {
    "x86_64-mingw": "mingw64",
    "i686-mingw": "mingw32",
}


# Cargo spells host triples its own way, and it is the only thing that can name a
# native target: `--target x86_64-pc-linux-gnu` is not a rust triple.
HOST_ARCH = "x86_64"
RUST_TRIPLE = {"x86_64": "x86_64-unknown-linux-gnu", "aarch64": "aarch64-unknown-linux-gnu"}


def is_system_file(text):
    """A system's generic.lua calls `return system({...})` at the top level.

    A substring test is not good enough: cairo's recipe comments on meson's
    `host_machine.system() == 'darwin'`, and that made the port treat a package
    as a system."""
    return re.search(r'(?m)^\s*return\s+system\s*\(', text) is not None


def is_recipe_file(text):
    return re.search(r'(?m)^\s*return\s+recipe\s*\(', text) is not None


# Which build systems the packages ask for, per build file (generic, or a system
# name). The systems pass reads this to know which driver scripts to write.
USES = {}


def rename(name):
    return RENAMES.get(name, name)


# Lines a system used to build its search paths, now the emitter's job.
CLOSURE_PATTERNS = [
    re.compile(r'^PKG_CONFIG_LIBDIR="\$PREFIX/(lib|share)/pkgconfig"\s*$'),
    re.compile(r'^PKG_CONFIG_LIBDIR="\$PKG_CONFIG_LIBDIR:\$PREFIX/(lib|share)/pkgconfig"\s*$'),
    re.compile(r'^LDFLAGS="-L\$PREFIX/lib"\s*$'),
    re.compile(r'^LDFLAGS="\$LDFLAGS -Wl,-rpath-link,\$PREFIX/lib"\s*$'),
    re.compile(r'^CMAKE_PREFIX_PATH="\$PREFIX"\s*$'),
    re.compile(r'^CMAKE_FLAGS="\$CMAKE_FLAGS -DCMAKE_PREFIX_PATH=\$PREFIX"\s*$'),
    re.compile(r'^CPPFLAGS="-I\$PREFIX/include"\s*$'),
    re.compile(r'^CFLAGS="\$CFLAGS -I\$PREFIX/include"\s*$'),
    re.compile(r'^CXXFLAGS="\$CXXFLAGS -I\$PREFIX/include"\s*$'),
    re.compile(r'^RUSTFLAGS="-L \$PREFIX/lib"\s*$'),
]

# Assignments whose right-hand side is a prefix the build system must be told about.
PREFIXY = re.compile(r'(--?prefix=|INSTALL_PREFIX=|CMAKE_INSTALL_PREFIX=|tooldir=|lib=|--docdir=|'
                     r'--with-pkg-config-libdir=|--with-tlibdir=|prefix=)')



# --- fixups ---------------------------------------------------------------
# Lines the port cannot resolve mechanically. Each one is a place where v1's single
# shared $PREFIX had to become something specific in v2, and the reason is recorded
# so the port stays auditable. Keys are "package/file.toml" or a package name.
FIXUPS = [
    # Each entry: which package and file, the text to find, what it becomes, and why.
    # v1's single shared $PREFIX is what makes almost all of these necessary: with
    # per-package directories each one has to name something specific instead.
    (("autoconf", "generic.toml"), r' --prefix="\$\{RETROLUNAR_PORT_PREFIX\}"', "",
     "AUTOCONF_CONFIGURE_FLAGS already carries --prefix=$OUTPKG"),
    (("automake", "generic.toml"), r' --prefix="\$\{RETROLUNAR_PORT_PREFIX\}"', "",
     "AUTOCONF_CONFIGURE_FLAGS already carries --prefix=$OUTPKG"),
    ("curl", r'--with-zlib="\$\{RETROLUNAR_PORT_PREFIX\}"', '--with-zlib="$zlib_PREFIX"',
     "zlib is a declared dependency with its own directory"),
    (("expect", "generic.toml"), r'--with-tcl="\$\{RETROLUNAR_PORT_PREFIX\}/lib"',
     '--with-tcl="$tcl_PREFIX/lib"', "tcl is a declared dependency"),
    (("expect", "generic.toml"), r'--with-tclinclude="\$\{RETROLUNAR_PORT_PREFIX\}/include"',
     '--with-tclinclude="$tcl_PREFIX/include"', "tcl is a declared dependency"),
    (("glad", "generic.toml"), r'prefix=\$\{RETROLUNAR_PORT_PREFIX\}', 'prefix="$OUTPKG"',
     "an install prefix: the final path, DESTDIR redirects the write"),
    (("imgui", "generic.toml"), r'prefix=\$\{RETROLUNAR_PORT_PREFIX\}', 'prefix="$OUTPKG"',
     "an install prefix: the final path, DESTDIR redirects the write"),
    (("lua", "generic.toml"), r'prefix=\$\{RETROLUNAR_PORT_PREFIX\}', 'prefix="$OUTPKG"',
     "an install prefix: the final path, DESTDIR redirects the write"),
    (("lcms2", "generic.toml"), r'--with-jpeg="\$\{RETROLUNAR_PORT_PREFIX\}"',
     '--with-jpeg="$libjpeg_turbo_PREFIX"', "libjpeg-turbo is a declared dependency"),
    (("lcms2", "generic.toml"), r'--with-tiff="\$\{RETROLUNAR_PORT_PREFIX\}"',
     '--with-tiff="$libtiff_PREFIX"', "libtiff is a declared dependency"),
    (("termcap", "generic.toml"), r'\$\{RETROLUNAR_PORT_PREFIX\}"', '"$OUTPKG"',
     "the .pc file has to record the final install path"),
    (("xml-parser", "generic.toml"), r'EXPATINCPATH=\$\{RETROLUNAR_PORT_PREFIX\}/include',
     'EXPATINCPATH="$expat_PREFIX/include"', "expat is a declared dependency"),
    (("xml-parser", "generic.toml"), r'EXPATLIBPATH=\$\{RETROLUNAR_PORT_PREFIX\}/lib',
     'EXPATLIBPATH="$expat_PREFIX/lib"', "expat is a declared dependency"),
    ("zstd", r'PREFIX="\$\{RETROLUNAR_PORT_PREFIX\}"', 'PREFIX="$OUTPKG"',
     "an install prefix: the final path, DESTDIR redirects the write"),
    ("zstd", r'LIBDIR="\$\{RETROLUNAR_PORT_PREFIX\}/lib"', 'LIBDIR="$OUTPKG/lib"',
     "an install prefix: the final path, DESTDIR redirects the write"),
    ("zstd", r' ZLIB_PREFIX="\$\{RETROLUNAR_PORT_PREFIX\}"', "",
     "HAVE_ZLIB=0, so the Makefile never reads it"),
    (("vim", "generic.toml"), r'--with-tlibdir="\$\{RETROLUNAR_PORT_PREFIX\}/lib"',
     '--with-tlibdir="$ncurses_PREFIX/lib"',
     "v1 took ncurses from the shared prefix and declared no dependency; v2 cannot"),
    (("perl", "generic.toml"), r'-D(privlib|archlib|sitelib|sitearch|vendorlib|vendorarch)='
     r'\$\{RETROLUNAR_PORT_PREFIX\}', r'-D\1=$OUTPKG',
     "perl bakes these paths in with no relocation, so they must be the final ones"),
    (("pngprobe", "generic.toml"), r'\\?\$\{RETROLUNAR_PORT_PREFIX\}',
     'a dependency directory', "a message, not a path"),
    (("libpng", "generic.toml"), r'for _f in "\$\{RETROLUNAR_PORT_PREFIX\}/lib"/libzlib\.\*; do\n'
     r'[^\n]*\n[^\n]*\n[^\n]*\ndone\n', "",
     "the mingw libz alias moved into zlib's own recipe, where the library is"),
    # The drivers pass these themselves from the system's own value, so v1's copy
    # of the same flag inside the flags string would only make it appear twice.
    (r'(^|-)mingw\d+$|^mingw\d+$',
     r'-DCMAKE_TOOLCHAIN_FILE=\$CMAKE_TOOLCHAIN_FILE ?', "",
     "the cmake driver passes the system's own CMAKE_TOOLCHAIN_FILE"),
    (r'(^|-)mingw\d+$|^mingw\d+$',
     r'--cross-file \$MESON_CROSS_FILE ?', "",
     "the meson driver passes the system's own MESON_CROSS_FILE"),
    (("expect", "generic.toml"),
     r'\$NATIVE_PREFIX/bin/tclsh8\.6', '$tcl_HOST_PREFIX/bin/tclsh8.6',
     "v1's shared native prefix is the host tcl's own directory in v2"),
    (("expect", "generic.toml"),
     r'# why it must not point at \$NATIVE_PREFIX\.',
     "# why it must not point at the host's tcl: $tcl_HOST_PREFIX.",
     "the comment named the v1 prefix"),
    (("libpng", "generic.toml"),
     r'# mingw zlib installs as libzlib \(not libz\), but configure\'s\n'
     r'# zlib probe hardcodes -lz \(LIBS env can\'t override it: the\n'
     r'# probe prepends its own -lz, and a missing -l is a hard error\)\.\n'
     r'# Alias the names in \$PREFIX so -lz resolves; harmless additive\n'
     r'# symlinks, benefits any later -lz user too\.',
     "# configure's zlib probe hardcodes -lz and prepends its own -lz, so LIBS cannot\n"
     "# override it and a missing -l is a hard error. On mingw, where zlib installs as\n"
     "# libzlib, the alias that makes -lz resolve lives in zlib's own mingw64.toml.",
     "the comment pointed at code that is a per-system recipe now"),
    (("libpng", "generic.toml"), r' --with-zlib-prefix="\$\{RETROLUNAR_PORT_PREFIX\}"', "",
     "the closure's -I and -L flags already point at zlib's directory"),
    # Systems: the same flag, in the two mingw cmake fragments.
    (r'^(mingw32|mingw64)$', r'-DZLIB_ROOT=\$\{RETROLUNAR_PORT_SYSTEM_PREFIX\}', "",
     "CMAKE_PREFIX_PATH from the closure points at the dependency directories already"),
]


# Files that have no v1 original because v1 did not need one: a recipe's
# platform-specific part, which v1 inlined behind a runtime test of the platform.
# In v2 it is the recipe of that system, and a regeneration must reproduce it.
EXTRA_FILES = {
    ("zlib", "mingw64.toml"): """deps = ["zlib@source"]
buildsystem = "cmake"
# Only the library is used; the examples cannot run on the build host.
args = ["-DZLIB_BUILD_EXAMPLES=OFF"]

# mingw zlib installs as libzlib, while configure scripts hardcode -lz, so the
# alias has to exist. It belongs in zlib's own directory, which is where the
# library is. v1 wrote it into the shared prefix (that is what prefixes were for)
# behind a runtime test for the platform; a recipe only the mingw system uses
# needs no test.
post = \"\"\"
for _f in \"$PREFIX/lib\"/libzlib.*; do
  [ -f \"$_f\" ] || continue
  _ext=\"${_f##*libzlib}\"
  [ -f \"$PREFIX/lib/libz$_ext\" ] || ln -s \"libzlib$_ext\" \"$PREFIX/lib/libz$_ext\"
done
\"\"\"
""",
}


def apply_fixups(pkg, fname, text):
    for key, pattern, repl, why in FIXUPS:
        if isinstance(key, tuple):
            want_pkg, want_file = key
            if pkg != want_pkg or fname != want_file:
                continue
        else:
            if not re.match(key + ("" if key.endswith("$") else "$"), pkg):
                continue
        new = re.sub(pattern, repl, text)
        if new != text:
            REPORT.add(f"{pkg}/{fname}", f"fixed by hand ({why})", pattern)
            text = new
    return text


class Report:
    def __init__(self):
        self.lines = []

    def add(self, path, what, text):
        self.lines.append((path, what, text.strip()[:200]))

    def write(self, path):
        with open(path, "w") as f:
            f.write("# Porting report\n\n")
            f.write("Every line the Lua→TOML port dropped, rewrote or flagged, per file.\n\n")
            cur = None
            for p, what, text in self.lines:
                if p != cur:
                    f.write(f"\n## {p}\n\n")
                    cur = p
                f.write(f"- **{what}**: `{text}`\n")


REPORT = Report()


# --- Lua reading ---------------------------------------------------------

def read_long_string(text, start):
    """Read a Lua long string starting at `start` (the first `[`), returning
    (value, end_index). Handles [[...]], [=[...]=] and deeper levels."""
    m = re.match(r'\[(=*)\[', text[start:])
    if not m:
        raise ValueError("not a long string")
    level = m.group(1)
    open_len = 2 + len(level)
    close = "]" + level + "]"
    body_start = start + open_len
    end = text.index(close, body_start)
    body = text[body_start:end]
    if body.startswith("\n"):
        body = body[1:]
    return body, end + len(close)


def lua_string(text, start):
    """Read a quoted Lua string, returning (value, end_index)."""
    assert text[start] == '"'
    out = []
    i = start + 1
    while i < len(text):
        c = text[i]
        if c == "\\":
            nxt = text[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(nxt, nxt))
            i += 2
            continue
        if c == '"':
            return "".join(out), i + 1
        out.append(c)
        i += 1
    raise ValueError("unterminated string")


def parse_recipe(path):
    """Return (fields, deps, order) for a `recipe({...})` or `system({...})` file."""
    text = open(path).read()
    deps = re.findall(r'require\(\s*"([^"]+)"\s*\)', text)
    # Fields: `key = value` at the top level of the table argument.
    fields = {}
    order = []
    body = text[text.index("{") + 1:] if "{" in text else ""
    i = 0
    while True:
        m = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*').search(body, i)
        if not m:
            break
        key = m.group(1)
        j = m.end()
        if body[j:j + 2] == "[[" or re.match(r'\[=', body[j:]):
            value, j = read_long_string(body, j)
        elif body[j] == '"':
            value, j = lua_string(body, j)
        elif body[j] == "{":
            depth, k = 0, j
            while k < len(body):
                if body[k] == "{":
                    depth += 1
                elif body[k] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            value = body[j:k + 1]
            j = k + 1
        else:
            k = j
            while k < len(body) and body[k] not in "\n":
                k += 1
            value = body[j:k].strip()
            j = k
        if key not in fields:
            order.append(key)
        fields[key] = value
        i = j
    return fields, deps, order


# --- body rewriting ------------------------------------------------------

def rewrite_body(body, path, for_system, own_name=None):
    """Rewrite a build body or a setup fragment for v2.

    `own_name` is the package's own name: in a source recipe v1 installed the
    unpacked tree into `$OUT/<own_name>`, and a source package's directory is
    that tree's home in v2, so the subdirectory goes away.
    """
    out = []
    for line in body.split("\n"):
        stripped = line.strip()
        # Shell comments: `--prefix=...` is an argument, not a comment, so only `#`
        # starts one here.
        comment = stripped.startswith("#")
        new = line
        flagged_prefix = for_system is None and re.search(r'\$PREFIX(?![A-Za-z0-9_])', new)

        if not comment:
            new = re.sub(r'\$NESTDIR/source/([A-Za-z0-9._+-]+)', r'$NESTDIR/\1@source', new)
            if own_name:
                new = re.sub(r'\$OUT/' + re.escape(own_name) + r'(?![A-Za-z0-9_-])', '$PREFIX',
                             new)
                new = re.sub(r'\$OUT(?![A-Za-z0-9_])', "$PREFIX", new)
                new = new.replace("$PREFIX/$PREFIX", "$PREFIX")

            # $OUT: the staging directory in v1. A prefix the build system is told about
            # must be the final path; everything else writes through $PREFIX.
            if "$OUT" in new and not own_name:
                if PREFIXY.search(new):
                    new = re.sub(r'\$OUT(?![A-Za-z0-9_])', "$OUTPKG", new)
                    REPORT.add(path, "rewrote $OUT to $OUTPKG (install prefix)", line)
                else:
                    new = re.sub(r'\$OUT(?![A-Za-z0-9_])', "$PREFIX", new)
                    REPORT.add(path, "rewrote $OUT to $PREFIX (staging)", line)

            # v1's $PREFIX meant "where the dependencies are". v2 has no such single
            # directory, so an unmapped use would silently mean the staging path.
            # Only occurrences that were already in the source are flagged: the
            # staging path this function writes is not one of them.
            if for_system and re.search(r'\$PREFIX(?![A-Za-z0-9_])', new):
                new = re.sub(r'\$PREFIX(?![A-Za-z0-9_])', "${RETROLUNAR_PORT_SYSTEM_PREFIX}", new)
                REPORT.add(path, "FLAGGED $PREFIX in a system fragment", line)
            if flagged_prefix:
                new = re.sub(r'\$PREFIX(?![A-Za-z0-9_])', "${RETROLUNAR_PORT_PREFIX}", new)
                REPORT.add(path, "FLAGGED $PREFIX meaning dependencies", line)

        if not comment and "$(command -v" in new:
            new = re.sub(r'\$\(command -v ([A-Za-z0-9_.+-]+)\)', r'$(command -v \1 || true)', new)
            REPORT.add(path, "made a tool lookup survive set -e", line)

        out.append(new)
    return "\n".join(out)


def toml_esc(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')


def dedent(text):
    """Strip the common leading indentation of a Lua long string body."""
    lines = text.split("\n")
    widths = [len(l) - len(l.lstrip(" ")) for l in lines if l.strip()]
    if not widths:
        return text
    cut = min(widths)
    return "\n".join(l[cut:] if l.strip() else "" for l in lines)


def body_to_toml(body):
    """Emit a shell body as a TOML basic multi-line string.

    Backslashes are escaped: TOML would otherwise turn a shell `\n` into a real
    newline, and these bodies are full of `printf '%s\n'` and awk programs."""
    text = dedent(body).rstrip("\n")
    return 'build = """\n' + toml_esc(text) + '\n"""\n'



# --- build systems -------------------------------------------------------
# What a build system is lives in the systems, not here: these templates are the
# text the port writes into each system's <name>.toml, and after that the file
# belongs to the system. Nothing in the C tool knows what cmake is.

BUILD_SYSTEMS = {
    "cmake": {
        "defaults": {"CMAKE_FLAGS": "", "CMAKE_TOOLCHAIN_FILE": "",
                     },
        # No generator is named: v1 never passed -G, and the one cmake picks
        # itself is the one that matches the tools the system actually pinned
        # (naming Ninja while pinning the host make is how a cross build stops).
        "script": """cmake -S "${BS_source_dir:-.}" -B build ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"} ${CMAKE_TOOLCHAIN_FILE:+-DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE"} $CMAKE_FLAGS $BS_args
cmake --build build --parallel "$CORES"
cmake --install build""",
        "doc": """# A recipe says `buildsystem = "cmake"` and gives only what is its own:
#   args        extra arguments, after the system's flags
#   source_dir  where CMakeLists.txt lives, when not the top of the work dir""",
    },
    "autoconf": {
        "defaults": {"AUTOCONF_CONFIGURE_FLAGS": ""},
        "script": """cd "${BS_source_dir:-.}"
./configure $AUTOCONF_CONFIGURE_FLAGS $BS_configure_args
for _f in aclocal.m4 configure config.h.in; do
  [ -f "$_f" ] && touch "$_f"
done
find . -name 'Makefile.in' -exec touch {} + 2>/dev/null || true
if [ "$BS_make" != "false" ]; then
  make -j"$CORES" $BS_make_args $BS_targets
fi
if [ "$BS_install" != "false" ]; then
  make $BS_install_args install
fi""",
        "doc": """# A recipe says `buildsystem = "autoconf"` and gives:
#   configure_args  extra arguments for ./configure
#   targets         arguments for the build step
#   install_args    arguments for the install step (default: install)
#
# The timestamps are touched before building because a release tarball unpacks
# with mtimes older than the configure/Makefile.in it ships, which makes make try
# to regenerate them with autotools - not installed in these systems, because
# building from a release tarball should not need them.""",
    },
    "meson": {
        "defaults": {"MESON_FLAGS": "", "MESON_CROSS_FILE": ""},
        "script": """meson setup build ${MESON_CROSS_FILE:+--cross-file "$MESON_CROSS_FILE"} $MESON_FLAGS $BS_args
ninja -C build -j "$CORES"
ninja -C build -j "$CORES" install""",
        "doc": """# A recipe says `buildsystem = "meson"` and gives:
#   args        extra -D options for setup
#   source_dir  where meson.build lives, when not the top of the work dir""",
    },
    "make": {
        "defaults": {},
        "script": """if [ "$BS_make" != "false" ]; then
  make -j"$CORES" CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" $BS_make_args $BS_targets
fi
if [ "$BS_install" != "false" ]; then
  DESTDIR="$DESTDIR" make CC="$CC" AR="$AR" RANLIB="$RANLIB" $BS_install_args install
fi""",
        "doc": """# A recipe says `buildsystem = "make"` and gives:
#   targets       what to build
#   make_args     arguments after the toolchain ones, so a recipe can override
#   install_args  what to install (default: install)
#   install       false when the build step already installs
#
# The toolchain is passed as arguments rather than through the environment
# because a Makefile that says `CC=gcc` ignores the environment and would build
# with the host's compiler.""",
    },
    "cargo": {
        "defaults": {"CARGO_BUILD_TARGET": ""},
        "script": """${BS_env:+env $BS_env }cargo build --release --target "$CARGO_BUILD_TARGET" -j "$CORES" ${BS_no_default_features:+--no-default-features} ${BS_features:+--features "$BS_features"}
if [ "$BS_install" != "false" ]; then
  mkdir -p "$PREFIX/bin"
  for _bin in ${BS_bin:?a cargo recipe must name its bin}; do
    _exe="target/$CARGO_BUILD_TARGET/release/$_bin"
    [ -f "$_exe" ] || _exe="$_exe.exe"
    cp "$_exe" "$PREFIX/bin/"
  done
fi""",
        "doc": """# A recipe says `buildsystem = "cargo"` and gives:
#   bin                  the binary to install (required)
#   features             --features value
#   no_default_features  true to add --no-default-features
#   install              false when something else installs the result
#
# Cargo does not read $LDFLAGS, so a binary it builds carries no runtime path;
# `retrolunar export` writes a launcher beside a bundle for those.""",
    },
    "python": {
        # No compilation: these packages are files to place, and their placement
        # is the same in every one of them. The site directory is a fact about the
        # system (it carries the interpreter's version), so eight recipes are not
        # each spelling it out and going stale when the version moves.
        # The version lives here, in the system, not in every recipe: it is a
        # fact about this host's interpreter.
        "defaults": {"PYTHON_SITE": "lib/python3.14/site-packages"},
        "script": """_site="$PREFIX/${PYTHON_SITE:?the system must say where its site-packages is}"
mkdir -p "$_site"
if [ "${BS_whole_tree:-}" != "" ]; then
  cp -r . "$_site/"
else
  for _m in ${BS_modules:?a python recipe must name its modules}; do
    cp -r "$_m" "$_site/"
  done
fi
for _l in ${BS_licenses:-}; do
  for _m in ${BS_modules:-}; do
    [ -f "$_l" ] && cp "$_l" "$_site/${_m##*/}/" || true
  done
done
for _s in ${BS_scripts:-}; do
  mkdir -p "$PREFIX/bin"
  cp "${_s%%:*}" "$PREFIX/bin/${_s##*:}"
  chmod +x "$PREFIX/bin/${_s##*:}"
done""",
        "doc": """# A recipe says `buildsystem = "python"` when a package is nothing but Python
# modules to place:
#   modules      directories to copy into site-packages
#   licenses     files copied into each module's directory
#   scripts      `source:name` pairs to install into bin/, made executable
#   whole_tree   true to copy the unpacked tree itself
# The system provides $PYTHON_SITE, `lib/python3.14/site-packages` on this host:
# the interpreter's version belongs in one place, not in every recipe.""",
    },
    "xmake": {
        "defaults": {"XMAKE_FLAGS": ""},
        "script": """xmake f -o "$OUTPKG" $XMAKE_FLAGS $BS_args
xmake -j "$CORES"
xmake install -o "$OUTPKG"
""",
        "doc": """# Nothing in these packages uses xmake yet: this is written from its manual.""",
    },
}

# Lines the drivers cover, and lines that are the tool's own prologue now.
CM_LINE = re.compile(r'^cmake\s+-S\s+(\S+)\s+-B\s+(\S+)\s+(.*)$')
CM_BUILD = re.compile(r'^cmake\s+--build\s+(\S+)')
CM_INSTALL = re.compile(r'^cmake\s+--install\s+')
CONFIGURE_LINE = re.compile(r'^\./configure\s+(.*)$')
MESON_SETUP = re.compile(r'^meson\s+setup\s+(\S+)\s+(.*)$')
NINJA_LINE = re.compile(r'^ninja\s+-C\s+(\S+)\s+(.*)$')
MAKE_LINE = re.compile(r'^make\s+(.*)$')
CARGO_LINE = re.compile(r'^((?:[A-Za-z_][A-Za-z0-9_]*="[^"]*"\s+)*)cargo\s+build\s+(.*)$')
TOUCH_FIX = re.compile(r'^(touch\s+aclocal\.m4|find\s+\.\s+-name\s+.Makefile\.in)')
SOURCE_COPY = re.compile(r'^cp\s+-r\s+\$NESTDIR/[A-Za-z0-9._+-]+@source/\*\s+\.\s*$')


SITE_MKDIR = re.compile(r'^mkdir -p "?\$PREFIX/([^"]*site-packages)"?$')
SITE_MODULE = re.compile(r'^cp -r (\S+) "?\$PREFIX/[^"]*site-packages/?$')
SITE_LICENSE = re.compile(r'^cp (\S+)((?: \S+)*) "?\$PREFIX/[^"]*site-packages/([^"/]+)(?:/[^"]*)?/?$')
SITE_WHOLE = re.compile(r'^cp -r \$NESTDIR/[A-Za-z0-9._+-]+@source/\* "?\$PREFIX/[^"]*site-packages/?$')
SITE_SCRIPT = re.compile(r'^cp (\S+) "?\$PREFIX/bin/([^"/]+)"?$')

PLAIN_MAKE = re.compile(r'^make(?: -j"\$CORES")?$')
PLAIN_INSTALL = re.compile(r'^make(?: -j"\$CORES")? install$')


def words(text):
    """Shell words of one command line, for the arguments a driver takes."""
    try:
        return shlex.split(text, comments=False, posix=True)
    except ValueError:
        return None


def join_continuations(lines):
    """`\` at the end of a line joins it to the next, as the shell would."""
    out = []
    pending = ""
    for line in lines:
        if line.rstrip().endswith("\\"):
            pending += line.rstrip()[:-1] + " "
            continue
        out.append(pending + line)
        pending = ""
    if pending:
        out.append(pending)
    return out


# --- packages ------------------------------------------------------------

def port_buildsystem(body, path):
    """Turn a v1 body into a build system plus the bits only this package has.

    Returns a dict with `name` (None when the body stays shell), `fields` for the
    recipe, `pre` and `build` (the lines before and after the driver's own), and
    `why` for the report. Conservative by design: a body it cannot account for
    completely stays a shell body, which is always correct.
    """
    lines = join_continuations(dedent(body).split("\n"))
    # The tool copies the source trees in now; the line that did it is noise.
    kept = []
    for line in lines:
        s = line.strip()
        if SOURCE_COPY.match(s):
            REPORT.add(path, "dropped (the tool copies @source trees in)", line)
            continue
        kept.append(line)
    lines = kept

    def commands(ls):
        return [(i, l.strip()) for i, l in enumerate(ls)
                if l.strip() and not l.strip().startswith("#")]

    cmds = commands(lines)
    if not cmds:
        return {"name": None, "fields": {}, "pre": [], "build": lines, "why": "no commands"}

    name = None
    for _, c in cmds:
        if CM_LINE.match(c):
            name = "cmake"
            break
        if CONFIGURE_LINE.match(c):
            name = "autoconf"
            break
        if MESON_SETUP.match(c):
            name = "meson"
            break
        if CARGO_LINE.match(c):
            name = "cargo"
            break
        if MAKE_LINE.match(c) and c.split()[0] == "make":
            name = "make"
            break

    drop = set()
    fields = {}

    # A package that is only Python modules to place: there is no build step at
    # all, and the shape repeats across the Python half of the tree.
    if any(SITE_MKDIR.match(c) or SITE_WHOLE.match(c) for _, c in cmds):
        fields = {}
        site = {"modules": [], "licenses": [], "scripts": []}
        ok = True
        for i, c in cmds:
            if SITE_MKDIR.match(c) or re.match(r'^chmod \+x ', c) or c == "mkdir -p $PREFIX/bin":
                drop.add(i)
                continue
            if SITE_WHOLE.match(c):
                fields["whole_tree"] = True
                drop.add(i)
                continue
            m = SITE_MODULE.match(c)
            if m:
                site["modules"].append(m.group(1))
                drop.add(i)
                continue
            m = SITE_LICENSE.match(c)
            if m:
                site["licenses"].append(m.group(1))
                for extra in m.group(2).split():
                    site["licenses"].append(extra)
                drop.add(i)
                continue
            m = SITE_SCRIPT.match(c)
            if m:
                site["scripts"].append("%s:%s" % (m.group(1), m.group(2)))
                drop.add(i)
                continue
            ok = False
            break
        if ok and (site["modules"] or fields.get("whole_tree")):
            for key, val in site.items():
                if val:
                    fields[key] = val
            if not fields.get("whole_tree"):
                fields.pop("whole_tree", None)
            first = min(drop) if drop else 0
            last = max(drop) if drop else 0
            return {"name": "python", "fields": fields,
                    "pre": [l for i, l in enumerate(lines)
                            if i < first and l.strip() and not l.strip().startswith("#")],
                    "build": [l for i, l in enumerate(lines)
                              if i > last and l.strip()],
                    "comments": [l.rstrip() for i, l in enumerate(lines)
                                 if i < first and l.strip().startswith("#")],
                    "why": "recognised"}

    toolchain = ("CC=", "AR=", "RANLIB=", "CFLAGS=", "CXXFLAGS=", "LDFLAGS=")
    install_lines = 0

    if name is None:
        # Say what the body is, not just that it is not a build system: this is
        # the queue a person reads when deciding what to do with it.
        text = "\n".join(c for _, c in cmds)
        if "BLOCKED" in text or "cannot be built" in text:
            why = "the recipe reports it cannot be built here"
        elif re.search(r'\$CC |\$CXX ', text) or "for _s in" in text and "$CC" in text:
            why = "a compilation of its own, not a build system"
        elif re.search(r'python3 \S+ (configure|setup)', text):
            why = "it bootstraps itself from python"
        elif re.search(r'\bmake\b', text):
            why = "make with arguments only the Makefile understands"
        else:
            why = "installing files is the whole recipe"
        return {"name": None, "fields": {}, "pre": [], "build": lines, "why": why}

    if name == "cmake":
        for i, c in cmds:
            m = CM_LINE.match(c)
            if m:
                src, _bdir, rest = m.group(1), m.group(2), m.group(3)
                toks = words(rest)
                if toks is None:
                    return {"name": None, "fields": {}, "pre": [], "build": lines,
                            "why": "unparseable cmake line"}
                if any(t == "-G" or t.startswith("-G") for t in toks):
                    return {"name": None, "fields": {}, "pre": [], "build": lines,
                            "why": "cmake picks its own generator"}
                args = [t for t in toks if t != "$CMAKE_FLAGS"]
                if args:
                    fields["args"] = args
                if src not in (".", "$PWD"):
                    fields["source_dir"] = src
                drop.add(i)
                continue
            if CM_BUILD.match(c) or CM_INSTALL.match(c):
                drop.add(i)
        if len([1 for _, c in cmds if CM_LINE.match(c)]) != 1:
            return {"name": None, "fields": {}, "pre": [], "build": lines,
                    "why": "more than one cmake invocation"}

    elif name == "autoconf":
        conf = [i for i, c in cmds if CONFIGURE_LINE.match(c)]
        if len(conf) != 1:
            return {"name": None, "fields": {}, "pre": [], "build": lines,
                    "why": "more than one configure invocation"}
        # The driver runs configure, make and make install. What a recipe adds
        # keeps its place: curl installs piece by piece, m4 builds its
        # subdirectories first, and upstream's Makefile is the only thing that
        # knows why. Only the pair the driver would run anyway is taken out.
        makes = [(i, c) for i, c in cmds if c.split()[0] == "make"]
        if len(makes) >= 2 and PLAIN_MAKE.match(makes[0][1]) and \
           PLAIN_INSTALL.match(makes[1][1]):
            drop.update({makes[0][0], makes[1][0]})
        elif makes:
            fields["make"] = False
            fields["install"] = False
        for i, c in cmds:
            m = CONFIGURE_LINE.match(c)
            if m:
                toks = words(m.group(1))
                if toks is None:
                    return {"name": None, "fields": {}, "pre": [], "build": lines,
                            "why": "unparseable configure line"}
                args = [t for t in toks if t != "$AUTOCONF_CONFIGURE_FLAGS"]
                if args:
                    fields["configure_args"] = args
                drop.add(i)
                continue
            if TOUCH_FIX.match(c):
                drop.add(i)
                continue

    elif name == "meson":
        for i, c in cmds:
            m = MESON_SETUP.match(c)
            if m:
                toks = words(m.group(2))
                if toks is None:
                    return {"name": None, "fields": {}, "pre": [], "build": lines,
                            "why": "unparseable meson line"}
                args = [t for t in toks if t != "$MESON_FLAGS"]
                if args:
                    fields["args"] = args
                drop.add(i)
                continue
            if NINJA_LINE.match(c):
                drop.add(i)

    elif name == "make":
        makes = [(i, c) for i, c in cmds if c.split()[0] == "make"]
        if len(makes) >= 2 and PLAIN_MAKE.match(makes[0][1]) and \
           PLAIN_INSTALL.match(makes[1][1]):
            drop.update({makes[0][0], makes[1][0]})
        elif makes:
            fields["make"] = False
            fields["install"] = False
        for i, c in cmds:
            if not (MAKE_LINE.match(c) and c.split()[0] == "make"):
                continue
            toks = words(c[5:].strip()) or []
            toks = [t for t in toks if t != '-j"$CORES"' and t != "-j$CORES"]
            if toks == ["install"]:
                drop.add(i)
                install_lines += 1
                continue
            vars_ = [t for t in toks if "=" in t or t.startswith("-")]
            rest = [t for t in toks if t not in vars_]
            if rest and rest[-1] == "install":
                # The build step installs too: no separate install command.
                fields["install"] = False
                fields["targets"] = rest[:-1] or None      # type: ignore[assignment]
                if fields["targets"] is None:
                    fields.pop("targets")
            elif rest:
                fields["targets"] = rest
            own = [t for t in vars_ if not any(t.startswith(p) for p in toolchain)]
            if own:
                fields["make_args"] = own
            drop.add(i)

    elif name == "cargo":
        for i, c in cmds:
            m = CARGO_LINE.match(c)
            if m:
                env = m.group(1).strip()
                if env:
                    # `env` sets these, and a shell word cannot carry quotes that
                    # came out of an expansion: keep the value bare.
                    fields.setdefault("env", re.sub(r'"([^"]*)"', r'\1', env))
                toks = words(m.group(2)) or []
                feats = None
                for k, t in enumerate(toks):
                    if t == "--features" and k + 1 < len(toks):
                        feats = toks[k + 1]
                    elif t == "--no-default-features":
                        fields["no_default_features"] = True
                    elif t in ("--release", "--target", "-j") or t.startswith("$CARGO_BUILD_TARGET"):
                        pass
                if feats:
                    fields["features"] = feats
                drop.add(i)
                continue
            m = re.match(r'^_bin="target/\$CARGO_BUILD_TARGET/release/([A-Za-z0-9._+-]+)"$', c)
            if m:
                fields["bin"] = m.group(1)
                drop.add(i)
                continue
            if re.match(r'^\[ -f "\$_bin" \]', c) or re.match(r'^mkdir -p "?\$PREFIX/bin', c) \
               or re.match(r'^cp "\$_bin" "\$PREFIX/bin/"$', c):
                drop.add(i)
                continue
        if "bin" not in fields:
            return {"name": None, "fields": {}, "pre": [], "build": lines,
                    "why": "a cargo recipe with no binary to install"}

    # Everything above the driver's own lines runs first, everything below runs
    # after it: that is what keeps a package's own preparation in order without
    # any of it being understood.
    first = min(drop)
    last = max(drop)
    pre = [l for i, l in enumerate(lines)
           if i < first and l.strip() and i not in drop and not l.strip().startswith("#")]
    build = [l for i, l in enumerate(lines) if i > last and l.strip() and i not in drop]
    comments = [l.rstrip() for i, l in enumerate(lines)
                if i < first and l.strip().startswith("#")]
    return {"name": name, "fields": fields, "pre": pre, "build": build,
            "comments": comments, "why": "recognised"}


def port_package(src_dir, dst_dir):
    name = os.path.basename(src_dir.rstrip("/"))
    os.makedirs(dst_dir, exist_ok=True)
    for entry in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, entry)
        if not entry.endswith(".lua"):
            if entry.endswith((".md",)):
                continue
            if os.path.isfile(path):
                shutil.copy2(path, os.path.join(dst_dir, entry))
                REPORT.add(entry, "copied unchanged", entry)
            continue
        if entry == "stage1.md":
            continue
        fields, deps, order = parse_recipe(path)
        out_name = rename(entry[:-4]) + ".toml"
        if entry == "generic.lua":
            out_name = "generic.toml"
        if entry == "source.lua":
            out_name = "source.toml"

        lines = []
        if out_name != "source.toml":
            if name == "vim" and "ncurses" not in deps:
                # v1 reached into the shared prefix for its terminal library and named no
                # dependency; with per-package directories that has to be explicit.
                deps = deps + ["ncurses"]
                REPORT.add(f"{name}/{entry}", "added a dependency v1 used implicitly",
                           "ncurses")
            if deps:
                lines.append("deps = [" + ", ".join('"%s"' % d for d in deps) + "]")
        for key in order:
            if key in ("build", "deps", "file", "dir", "sys", "system", "name"):
                continue
            lines.append('%s = "%s"' % (key, toml_esc(fields[key])))
        if "build" in fields and out_name != "source.toml":
            own = name if out_name == "source.toml" else None
            prepared = rewrite_body(dedent(fields["build"]), f"{name}/{entry}", None, own)
            prepared = apply_fixups(name, out_name, prepared)
            bs = port_buildsystem(prepared, f"{name}/{entry}")
            if bs["name"] is not None:
                USES.setdefault(entry[:-4], set()).add(bs["name"])
                lines.append('buildsystem = "%s"' % bs["name"])
                for c in bs.get("comments", []):
                    if c.strip() and not c.strip().startswith("# source") :
                        lines.append(c.strip())
                for k, v in bs["fields"].items():
                    if isinstance(v, bool):
                        lines.append("%s = %s" % (k, "true" if v else "false"))
                    elif isinstance(v, list):
                        lines.append("%s = [%s]" % (k, ", ".join('"%s"' % toml_esc(x) for x in v)))
                    else:
                        lines.append('%s = "%s"' % (k, toml_esc(v)))
                for key, label in (("pre", "pre"), ("build", "post")):
                    part = bs["pre"] if key == "pre" else bs["build"]
                    if part:
                        text = "\n".join(part).rstrip("\n")
                        lines.append('%s = """' % label)
                        lines.append(toml_esc(text))
                        lines.append('"""')
                REPORT.add(f"{name}/{entry}", "driven by the %s build system" % bs["name"],
                           ", ".join("%s=%s" % kv for kv in bs["fields"].items()) or "no arguments")
                text = "\n".join(lines).rstrip("\n") + "\n"
                with open(os.path.join(dst_dir, out_name), "w") as f:
                    f.write(text)
                REPORT.add(f"{name}/{entry}", "ported", out_name)
                continue
            REPORT.add(f"{name}/{entry}", "left as a shell body (%s)" % bs["why"],
                       entry)

        if "build" in fields:
            lines.append("")
            own = name if out_name == "source.toml" else None
            body = rewrite_body(dedent(fields["build"]), f"{name}/{entry}", None, own)
            # Fixups see the shell text, with its quoting, not the TOML escaping of it.
            body = apply_fixups(name, out_name, body)
            lines.append(body_to_toml(body))

        text = "\n".join(lines).rstrip("\n") + "\n"
        with open(os.path.join(dst_dir, out_name), "w") as f:
            f.write(text)
        REPORT.add(f"{name}/{entry}", "ported", out_name)
        for (want_pkg, want_file), body in EXTRA_FILES.items():
            if want_pkg != name:
                continue
            with open(os.path.join(dst_dir, want_file), "w") as f:
                f.write(body)
            REPORT.add(f"{name}/{want_file}", "written from the port's own notes",
                       "v1 had no separate file for this system")


# --- systems -------------------------------------------------------------

def shell_line(s):
    """Is this a line the shell has to run, rather than a plain value?"""
    return (s.startswith(":") or re.match(r'^(if|for|while|case|unset|exit|_|eval)\b', s)
            or s.endswith("fi") or s.endswith("done") or s.endswith("then") or s == "fi"
            or s.startswith("_ndk")
            or ("$(" in s and re.match(r'^[A-Za-z_]+="\$\(', s) is None))


def read_names(text):
    """Names a line reads."""
    return {m.group(1) for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)', text)}


def assigned_names(line):
    """Names a shell line assigns, so a later value can be kept next to it."""
    names = set()
    for m in re.finditer(r'(?:^|\s|;)(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)="?', line):
        names.add(m.group(1))
    return names


def references_any(text, names):
    for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)', text):
        if m.group(1) in names:
            return True
    return False


def split_sections(setup):
    """Split a v1 setup fragment into (marker, [lines]) chunks."""
    sections = []
    current = ("(header)", [])
    for line in setup.split("\n"):
        m = re.match(r'\s*#\s*---\s*(.+?)\s*-*\s*$', line)
        if m:
            if current[1]:
                sections.append(current)
            current = (m.group(1), [])
            continue
        current[1].append(line)
    if current[1]:
        sections.append(current)
    return sections


def section_kind(marker, line):
    """Which v2 file a setup line belongs in."""
    s = line.strip()
    if s.startswith("#") or not s:
        return "comment"
    m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)=', s)
    var = m.group(1) if m else ""
    for prefix, bs in BUILDSYSTEM_BY_PREFIX:
        if var.startswith(prefix):
            return bs
    # `ac_cv_*`/`gl_cv_*` are configure's cache: they belong next to the rest of
    # autoconf's settings, not in the system's base facts.
    if re.match(r'^(ac|gl|gt)_cv_', var) or re.match(r'^[a-z_]+_cv_', var):
        return "autoconf"
    if var in ("CC_aarch64_linux_android", "CFLAGS_aarch64_linux_android",
               "CXX_aarch64_linux_android", "CXXFLAGS_aarch64_linux_android") or \
       re.match(r'(CC|CFLAGS|CXX|CXXFLAGS)_[a-z0-9_]+$', var):
        return "cargo"
    if var:
        return "system"
    return "shell"


def port_system(src_dir, dst_root, native_name, uses_path=None):
    name = rename(os.path.basename(src_dir.rstrip("/")))
    out_dir = os.path.join(dst_root, "system@" + name)
    os.makedirs(out_dir, exist_ok=True)

    fields, deps, order = parse_recipe(os.path.join(src_dir, "generic.lua"))
    fallbacks = [rename(f) for f in re.findall(r'"([^"]+)"', fields.get("recipe_fallbacks", ""))]

    buckets = {"system": [], "autoconf": [], "cmake": [], "meson": [], "cargo": [],
               "make": [], "python": [], "xmake": [], "script": []}
    script_names = set()
    pending = []
    assigns = {}
    var_kind = {}
    var_comments = {}
    var_order = []

    # The shell part is emitted before the values, so a name the shell reads or
    # computes cannot be a value, and neither can a value that uses one of those
    # (`CFLAGS` is appended to after `$SYSROOT` exists). Work that out first, over
    # the whole fragment, then bucket with the answer in hand.
    fragment = dedent(fields.get("setup", ""))
    shell_names = set()
    for marker, lines in split_sections(fragment):
        for line in lines:
            t = line.strip()
            if not t or section_kind(marker, t) == "comment":
                continue
            if shell_line(t) or section_kind(marker, t) == "shell":
                shell_names |= read_names(t) | assigned_names(t)
    grow = True
    while grow:
        grow = False
        for marker, lines in split_sections(fragment):
            for line in lines:
                t = line.strip()
                m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)="(.*)"\s*$', t)
                if m and m.group(1) not in shell_names and references_any(m.group(2), shell_names):
                    shell_names.add(m.group(1))
                    grow = True

    for marker, lines in split_sections(fragment):
        for line in lines:
            if not line.strip():
                continue
            kind = section_kind(marker, line)
            s = line.strip()
            if kind == "comment":
                pending.append(s)
                continue
            # Search paths the emitter now builds from the closure.
            if any(p.match(s) for p in CLOSURE_PATTERNS):
                REPORT.add(f"{name}/generic.lua", "dropped (closure provides it)", s)
                continue
            if s.startswith("export ") and "=" in s.split(None, 1)[1]:
                s = s.split(None, 1)[1]
                line = s
                REPORT.add(f"{name}/generic.lua",
                           "kept an exported assignment (v2 exports every variable)", s)
            elif s.startswith("export "):
                REPORT.add(f"{name}/generic.lua",
                           "dropped 'export' (variables are exported per file)", s)
                continue
            if re.match(r'^CORES=', s):
                REPORT.add(f"{name}/generic.lua", "dropped CORES (the emitter exports it)", s)
                continue
            if shell_line(s):
                buckets["script"].append(line)
                script_names.update(assigned_names(s))
                continue
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)="(.*)"\s*$', s)
            if m:
                var, value = m.group(1), m.group(2)
                kind = section_kind(marker, s)
                if kind == "shell":
                    buckets["script"].extend(pending)
                    del pending[:]
                    buckets["script"].append(line)
                    continue
                value = rewrite_body(value, f"{name}/generic.lua", True)
                # A value that uses something the shell part computed has to stay
                # where it is: the fragment is a sequence (`_ndk_ver` is set by the
                # NDK discovery and unset again right after `NDK` uses it), and
                # moving the values into their own block breaks it.
                if var in shell_names:
                    buckets["script"].extend(pending)
                    del pending[:]
                    buckets["script"].append(line)
                    continue
                if var not in assigns:
                    assigns[var] = []
                    var_kind[var] = kind
                    var_comments[var] = list(pending)
                    del pending[:]
                    var_order.append(var)
                assigns[var].append(value)
                continue
            buckets["script"].extend(pending)
            del pending[:]
            buckets["script"].append(line)

    # v1 set a variable once and then appended to it; TOML allows one key, so the
    # assignments are merged. A variable the emitter also sets has to keep its own
    # value in front of the appended parts, or the closure's search paths vanish.
    EMITTER_VARS = {"CPPFLAGS", "LDFLAGS", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_PATH",
                    "CMAKE_PREFIX_PATH", "PATH", "LD_LIBRARY_PATH", "DEPS"}
    for var in var_order:
        parts = assigns[var]
        keep_self = var in EMITTER_VARS or parts[0].lstrip().startswith("$" + var)
        merged = ""
        for raw in parts:
            piece = raw.strip()
            if piece.startswith("$" + var):
                piece = piece[len(var) + 1:]
            if not piece:
                continue
            if merged and merged[-1] not in ":; " and piece[0] not in ":; ":
                merged += " "
            merged += piece
        while "  " in merged:
            merged = merged.replace("  ", " ")
        value = apply_fixups(name, var_kind[var] + ".toml", merged)
        if keep_self:
            sep = "" if merged[:1] in (":", ";") or not merged else " "
            value = "$" + var + sep + merged.strip()
        buckets[var_kind[var]].extend(var_comments[var])
        buckets[var_kind[var]].append(f'{var} = "{toml_esc(value)}"')
        if len(parts) > 1:
            REPORT.add(f"{name}/generic.lua", "merged repeated assignments", var)

    # system.toml
    sys_lines = []
    if fallbacks:
        sys_lines.append("fallbacks = [" + ", ".join('"%s"' % f for f in fallbacks) + "]")
    # No host key: v1 had none, and it needs none. A build that needs a host
    # program either names it as a dependency (`m4@native`, whose directory the
    # emitter puts on PATH) or uses the one already in the user's PATH, which is
    # what every v1 cross system did.
    if buckets["script"]:
        script = toml_esc("\n".join(buckets["script"] + pending))
        sys_lines.append("")
        sys_lines.append('script = """\n' + script.strip("\n") + '\n"""')
    if buckets["system"]:
        sys_lines.append("")
        sys_lines.extend(buckets["system"])
    with open(os.path.join(out_dir, "system.toml"), "w") as f:
        f.write("[system]\n" + "\n".join(sys_lines).rstrip("\n") + "\n")

    if name == native_name and not buckets["cargo"]:
        # v1's native system defined no cargo target, and its cargo recipes pass
        # `--target $CARGO_BUILD_TARGET`: a native cargo build could not have worked.
        # Cargo has its own triple spelling, so this is not $TARGET_TRIPLET.
        buckets["cargo"].append('CARGO_BUILD_TARGET = "%s"' % RUST_TRIPLE.get(HOST_ARCH, ""))
        REPORT.add(f"{name}/generic.lua", "added the cargo target v1 never defined",
                   RUST_TRIPLE.get(HOST_ARCH, ""))
    # Which build systems the packages of this system ask for: read from the
    # manifest the packages pass wrote. A system carries the driver script for
    # each, in its own file, where its fingerprint covers it.
    used = set()
    uses_file = uses_path or os.path.join(dst_root, ".port-uses.json")
    if os.path.isfile(uses_file):
        try:
            with open(uses_file) as f:
                manifest = json.load(f)
            for key in ("generic", name):
                used.update(manifest.get(key, []))
        except (ValueError, OSError):
            used = set()

    for bs in ("autoconf", "cmake", "meson", "cargo", "make", "python", "xmake"):
        body = buckets[bs]
        tpl = BUILD_SYSTEMS.get(bs) if bs in used else None
        if not body and tpl is None:
            continue
        parts = []
        if tpl is not None and tpl.get("doc"):
            parts.append(tpl["doc"])
        parts.append("[buildsystem.%s]" % bs)
        if tpl is not None:
            have = "\n".join(body)
            for key, value in tpl["defaults"].items():
                if re.search(r'(?m)^%s = ' % re.escape(key), have):
                    continue  # the system says its own value
                parts.append('%s = "%s"' % (key, toml_esc(value)))
        parts.extend(body)
        if tpl is not None:
            parts.append('script = """')
            parts.append(toml_esc(tpl["script"].rstrip("\n")))
            parts.append('"""')
        with open(os.path.join(out_dir, bs + ".toml"), "w") as f:
            f.write("\n".join(parts).rstrip("\n") + "\n")
        if tpl is not None:
            REPORT.add(f"system@{name}/{bs}.toml", "carries the %s driver" % bs,
                       "script + defaults, overridable by this system")
        else:
            REPORT.add(f"system@{name}/{bs}.toml", "system's own %s settings" % bs,
                       "%d line(s)" % len(body))

    for entry in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, entry)
        if not entry.endswith(".lua"):
            if entry.endswith((".md",)):
                continue
            if os.path.isfile(path):
                shutil.copy2(path, os.path.join(dst_dir, entry))
                REPORT.add(entry, "copied unchanged", entry)
            continue
        if entry == "stage1.md":
            continue
        fields, deps, order = parse_recipe(path)
        out_name = rename(entry[:-4]) + ".toml"
        if entry == "generic.lua":
            out_name = "generic.toml"
        if entry == "source.lua":
            out_name = "source.toml"

        lines = []
        if out_name != "source.toml":
            if name == "vim" and "ncurses" not in deps:
                # v1 reached into the shared prefix for its terminal library and named no
                # dependency; with per-package directories that has to be explicit.
                deps = deps + ["ncurses"]
                REPORT.add(f"{name}/{entry}", "added a dependency v1 used implicitly",
                           "ncurses")
            if deps:
                lines.append("deps = [" + ", ".join('"%s"' % d for d in deps) + "]")
        for key in order:
            if key in ("build", "deps", "file", "dir", "sys", "system", "name"):
                continue
            lines.append('%s = "%s"' % (key, toml_esc(fields[key])))
        if "build" in fields and out_name != "source.toml":
            own = name if out_name == "source.toml" else None
            prepared = rewrite_body(dedent(fields["build"]), f"{name}/{entry}", None, own)
            prepared = apply_fixups(name, out_name, prepared)
            bs = port_buildsystem(prepared, f"{name}/{entry}")
            if bs["name"] is not None:
                USES.setdefault(entry[:-4], set()).add(bs["name"])
                lines.append('buildsystem = "%s"' % bs["name"])
                for c in bs.get("comments", []):
                    if c.strip() and not c.strip().startswith("# source") :
                        lines.append(c.strip())
                for k, v in bs["fields"].items():
                    if isinstance(v, bool):
                        lines.append("%s = %s" % (k, "true" if v else "false"))
                    elif isinstance(v, list):
                        lines.append("%s = [%s]" % (k, ", ".join('"%s"' % toml_esc(x) for x in v)))
                    else:
                        lines.append('%s = "%s"' % (k, toml_esc(v)))
                for key, label in (("pre", "pre"), ("build", "post")):
                    part = bs["pre"] if key == "pre" else bs["build"]
                    if part:
                        text = "\n".join(part).rstrip("\n")
                        lines.append('%s = """' % label)
                        lines.append(toml_esc(text))
                        lines.append('"""')
                REPORT.add(f"{name}/{entry}", "driven by the %s build system" % bs["name"],
                           ", ".join("%s=%s" % kv for kv in bs["fields"].items()) or "no arguments")
                text = "\n".join(lines).rstrip("\n") + "\n"
                with open(os.path.join(dst_dir, out_name), "w") as f:
                    f.write(text)
                REPORT.add(f"{name}/{entry}", "ported", out_name)
                continue
            REPORT.add(f"{name}/{entry}", "left as a shell body (%s)" % bs["why"],
                       entry)

        if "build" in fields:
            lines.append("")
            own = name if out_name == "source.toml" else None
            body = rewrite_body(dedent(fields["build"]), f"{name}/{entry}", None, own)
            # Fixups see the shell text, with its quoting, not the TOML escaping of it.
            body = apply_fixups(name, out_name, body)
            lines.append(body_to_toml(body))

        text = "\n".join(lines).rstrip("\n") + "\n"
        with open(os.path.join(dst_dir, out_name), "w") as f:
            f.write(text)
        REPORT.add(f"{name}/{entry}", "ported", out_name)
        for (want_pkg, want_file), body in EXTRA_FILES.items():
            if want_pkg != name:
                continue
            with open(os.path.join(dst_dir, want_file), "w") as f:
                f.write(body)
            REPORT.add(f"{name}/{want_file}", "written from the port's own notes",
                       "v1 had no separate file for this system")


# --- systems -------------------------------------------------------------

def shell_line(s):
    """Is this a line the shell has to run, rather than a plain value?"""
    return (s.startswith(":") or re.match(r'^(if|for|while|case|unset|exit|_|eval)\b', s)
            or s.endswith("fi") or s.endswith("done") or s.endswith("then") or s == "fi"
            or s.startswith("_ndk")
            or ("$(" in s and re.match(r'^[A-Za-z_]+="\$\(', s) is None))


def read_names(text):
    """Names a line reads."""
    return {m.group(1) for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)', text)}


def assigned_names(line):
    """Names a shell line assigns, so a later value can be kept next to it."""
    names = set()
    for m in re.finditer(r'(?:^|\s|;)(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)="?', line):
        names.add(m.group(1))
    return names


def references_any(text, names):
    for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)', text):
        if m.group(1) in names:
            return True
    return False


def split_sections(setup):
    """Split a v1 setup fragment into (marker, [lines]) chunks."""
    sections = []
    current = ("(header)", [])
    for line in setup.split("\n"):
        m = re.match(r'\s*#\s*---\s*(.+?)\s*-*\s*$', line)
        if m:
            if current[1]:
                sections.append(current)
            current = (m.group(1), [])
            continue
        current[1].append(line)
    if current[1]:
        sections.append(current)
    return sections


def section_kind(marker, line):
    """Which v2 file a setup line belongs in."""
    s = line.strip()
    if s.startswith("#") or not s:
        return "comment"
    m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)=', s)
    var = m.group(1) if m else ""
    for prefix, bs in BUILDSYSTEM_BY_PREFIX:
        if var.startswith(prefix):
            return bs
    # `ac_cv_*`/`gl_cv_*` are configure's cache: they belong next to the rest of
    # autoconf's settings, not in the system's base facts.
    if re.match(r'^(ac|gl|gt)_cv_', var) or re.match(r'^[a-z_]+_cv_', var):
        return "autoconf"
    if var in ("CC_aarch64_linux_android", "CFLAGS_aarch64_linux_android",
               "CXX_aarch64_linux_android", "CXXFLAGS_aarch64_linux_android") or \
       re.match(r'(CC|CFLAGS|CXX|CXXFLAGS)_[a-z0-9_]+$', var):
        return "cargo"
    if var:
        return "system"
    return "shell"


def port_system(src_dir, dst_root, native_name, uses_path=None):
    name = rename(os.path.basename(src_dir.rstrip("/")))
    out_dir = os.path.join(dst_root, "system@" + name)
    os.makedirs(out_dir, exist_ok=True)

    fields, deps, order = parse_recipe(os.path.join(src_dir, "generic.lua"))
    fallbacks = [rename(f) for f in re.findall(r'"([^"]+)"', fields.get("recipe_fallbacks", ""))]

    buckets = {"system": [], "autoconf": [], "cmake": [], "meson": [], "cargo": [],
               "make": [], "python": [], "xmake": [], "script": []}
    script_names = set()
    pending = []
    assigns = {}
    var_kind = {}
    var_comments = {}
    var_order = []

    # The shell part is emitted before the values, so a name the shell reads or
    # computes cannot be a value, and neither can a value that uses one of those
    # (`CFLAGS` is appended to after `$SYSROOT` exists). Work that out first, over
    # the whole fragment, then bucket with the answer in hand.
    fragment = dedent(fields.get("setup", ""))
    shell_names = set()
    for marker, lines in split_sections(fragment):
        for line in lines:
            t = line.strip()
            if not t or section_kind(marker, t) == "comment":
                continue
            if shell_line(t) or section_kind(marker, t) == "shell":
                shell_names |= read_names(t) | assigned_names(t)
    grow = True
    while grow:
        grow = False
        for marker, lines in split_sections(fragment):
            for line in lines:
                t = line.strip()
                m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)="(.*)"\s*$', t)
                if m and m.group(1) not in shell_names and references_any(m.group(2), shell_names):
                    shell_names.add(m.group(1))
                    grow = True

    for marker, lines in split_sections(fragment):
        for line in lines:
            if not line.strip():
                continue
            kind = section_kind(marker, line)
            s = line.strip()
            if kind == "comment":
                pending.append(s)
                continue
            # Search paths the emitter now builds from the closure.
            if any(p.match(s) for p in CLOSURE_PATTERNS):
                REPORT.add(f"{name}/generic.lua", "dropped (closure provides it)", s)
                continue
            if s.startswith("export ") and "=" in s.split(None, 1)[1]:
                s = s.split(None, 1)[1]
                line = s
                REPORT.add(f"{name}/generic.lua",
                           "kept an exported assignment (v2 exports every variable)", s)
            elif s.startswith("export "):
                REPORT.add(f"{name}/generic.lua",
                           "dropped 'export' (variables are exported per file)", s)
                continue
            if re.match(r'^CORES=', s):
                REPORT.add(f"{name}/generic.lua", "dropped CORES (the emitter exports it)", s)
                continue
            if shell_line(s):
                buckets["script"].append(line)
                script_names.update(assigned_names(s))
                continue
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)="(.*)"\s*$', s)
            if m:
                var, value = m.group(1), m.group(2)
                kind = section_kind(marker, s)
                if kind == "shell":
                    buckets["script"].extend(pending)
                    del pending[:]
                    buckets["script"].append(line)
                    continue
                value = rewrite_body(value, f"{name}/generic.lua", True)
                # A value that uses something the shell part computed has to stay
                # where it is: the fragment is a sequence (`_ndk_ver` is set by the
                # NDK discovery and unset again right after `NDK` uses it), and
                # moving the values into their own block breaks it.
                if var in shell_names:
                    buckets["script"].extend(pending)
                    del pending[:]
                    buckets["script"].append(line)
                    continue
                if var not in assigns:
                    assigns[var] = []
                    var_kind[var] = kind
                    var_comments[var] = list(pending)
                    del pending[:]
                    var_order.append(var)
                assigns[var].append(value)
                continue
            buckets["script"].extend(pending)
            del pending[:]
            buckets["script"].append(line)

    # v1 set a variable once and then appended to it; TOML allows one key, so the
    # assignments are merged. A variable the emitter also sets has to keep its own
    # value in front of the appended parts, or the closure's search paths vanish.
    EMITTER_VARS = {"CPPFLAGS", "LDFLAGS", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_PATH",
                    "CMAKE_PREFIX_PATH", "PATH", "LD_LIBRARY_PATH", "DEPS"}
    for var in var_order:
        parts = assigns[var]
        keep_self = var in EMITTER_VARS or parts[0].lstrip().startswith("$" + var)
        merged = ""
        for raw in parts:
            piece = raw.strip()
            if piece.startswith("$" + var):
                piece = piece[len(var) + 1:]
            if not piece:
                continue
            if merged and merged[-1] not in ":; " and piece[0] not in ":; ":
                merged += " "
            merged += piece
        while "  " in merged:
            merged = merged.replace("  ", " ")
        value = apply_fixups(name, var_kind[var] + ".toml", merged)
        if keep_self:
            sep = "" if merged[:1] in (":", ";") or not merged else " "
            value = "$" + var + sep + merged.strip()
        buckets[var_kind[var]].extend(var_comments[var])
        buckets[var_kind[var]].append(f'{var} = "{toml_esc(value)}"')
        if len(parts) > 1:
            REPORT.add(f"{name}/generic.lua", "merged repeated assignments", var)

    # system.toml
    sys_lines = []
    if fallbacks:
        sys_lines.append("fallbacks = [" + ", ".join('"%s"' % f for f in fallbacks) + "]")
    # No host key: v1 had none, and it needs none. A build that needs a host
    # program either names it as a dependency (`m4@native`, whose directory the
    # emitter puts on PATH) or uses the one already in the user's PATH, which is
    # what every v1 cross system did.
    if buckets["script"]:
        script = toml_esc("\n".join(buckets["script"] + pending))
        sys_lines.append("")
        sys_lines.append('script = """\n' + script.strip("\n") + '\n"""')
    if buckets["system"]:
        sys_lines.append("")
        sys_lines.extend(buckets["system"])
    with open(os.path.join(out_dir, "system.toml"), "w") as f:
        f.write("[system]\n" + "\n".join(sys_lines).rstrip("\n") + "\n")

    if name == native_name and not buckets["cargo"]:
        # v1's native system defined no cargo target, and its cargo recipes pass
        # `--target $CARGO_BUILD_TARGET`: a native cargo build could not have worked.
        # Cargo has its own triple spelling, so this is not $TARGET_TRIPLET.
        buckets["cargo"].append('CARGO_BUILD_TARGET = "%s"' % RUST_TRIPLE.get(HOST_ARCH, ""))
        REPORT.add(f"{name}/generic.lua", "added the cargo target v1 never defined",
                   RUST_TRIPLE.get(HOST_ARCH, ""))
    # Which build systems the packages of this system ask for: read from the
    # manifest the packages pass wrote. A system carries the driver script for
    # each, in its own file, where its fingerprint covers it.
    used = set()
    uses_file = uses_path or os.path.join(dst_root, ".port-uses.json")
    if os.path.isfile(uses_file):
        try:
            with open(uses_file) as f:
                manifest = json.load(f)
            for key in ("generic", name):
                used.update(manifest.get(key, []))
        except (ValueError, OSError):
            used = set()

    for bs in sorted(used):
        tpl = BUILD_SYSTEMS.get(bs)
        if tpl is None:
            continue
        have = "\n".join(buckets[bs])
        parts = []
        if tpl.get("doc"):
            parts.append(tpl["doc"])
        parts.append("[buildsystem.%s]" % bs)
        for key, value in tpl["defaults"].items():
            if re.search(r'(?m)^%s = ' % re.escape(key), have):
                continue  # the system says its own value
            parts.append('%s = "%s"' % (key, toml_esc(value)))
        parts.extend(buckets[bs])
        script = tpl["script"].rstrip("\n")
        parts.append('script = """')
        parts.append(toml_esc(script))
        parts.append('"""')
        with open(os.path.join(out_dir, bs + ".toml"), "w") as f:
            f.write("\n".join(parts).rstrip("\n") + "\n")
        REPORT.add(f"system@{name}/{bs}.toml", "carries the %s driver" % bs,
                   "script + defaults, overridable by this system")
        buckets[bs] = []

    for entry in sorted(os.listdir(src_dir)):
        if entry.endswith((".cmake", ".ini")):
            out_entry = entry
            for old, new in RENAMES.items():
                out_entry = out_entry.replace(old, new)
            if out_entry != entry:
                REPORT.add(f"{name}/{entry}", "renamed with the system", out_entry)
            shutil.copy2(os.path.join(src_dir, entry), os.path.join(out_dir, out_entry))
            REPORT.add(f"{name}/{entry}", "copied unchanged", entry)
    REPORT.add(f"{name}/generic.lua", "ported to", "system@" + name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["systems", "packages"])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--native", default="clang-native")
    ap.add_argument("--report", default="PORTING.md")
    ap.add_argument("--uses", default=None,
                    help="manifest from the packages pass, naming the build "
                         "systems each system's packages need")
    args = ap.parse_args()

    if args.mode == "systems":
        for entry in sorted(os.listdir(args.src)):
            path = os.path.join(args.src, entry)
            g = os.path.join(path, "generic.lua")
            if not os.path.isdir(path) or not os.path.isfile(g):
                continue
            if not is_system_file(open(g).read()):
                continue
            port_system(path, args.dst, args.native, args.uses)
    else:
        for entry in sorted(os.listdir(args.src)):
            path = os.path.join(args.src, entry)
            g = os.path.join(path, "generic.lua")
            if not os.path.isdir(path):
                continue
            if os.path.isfile(g) and is_system_file(open(g).read()):
                continue  # a system, not a package
            if not os.path.isfile(g) and not os.path.isfile(os.path.join(path, "source.lua")):
                continue
            port_package(path, os.path.join(args.dst, entry))

    if args.mode == "packages":
        with open(os.path.join(args.dst, ".port-uses.json"), "w") as f:
            json.dump({k: sorted(v) for k, v in USES.items()}, f, indent=1, sort_keys=True)
    if args.mode == "systems" and args.uses is None:
        default_manifest = os.path.join(args.dst, ".port-uses.json")
        if os.path.isfile(default_manifest):
            os.remove(default_manifest)
    REPORT.write(args.report)
    print(f"wrote {args.report}: {len(REPORT.lines)} entries", file=sys.stderr)


if __name__ == "__main__":
    main()
