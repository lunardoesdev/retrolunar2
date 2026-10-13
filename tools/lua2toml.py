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
    (("libpng", "generic.toml"), r' --with-zlib-prefix="\$\{RETROLUNAR_PORT_PREFIX\}"', "",
     "the closure's -I and -L flags already point at zlib's directory"),
    # Systems: the same flag, in the two mingw cmake fragments.
    (r'^(mingw32|mingw64)$', r'-DZLIB_ROOT=\$\{RETROLUNAR_PORT_SYSTEM_PREFIX\}', "",
     "CMAKE_PREFIX_PATH from the closure points at the dependency directories already"),
]


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


# --- packages ------------------------------------------------------------

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
        if "build" in fields:
            lines.append("")
            own = name if out_name == "source.toml" else None
            body = rewrite_body(dedent(fields["build"]), f"{name}/{entry}", None, own)
            if name == "zlib" and out_name == "generic.toml":
                # Added after the rewrite: this text is already v2 and must not be
                # flagged as a v1 $PREFIX.
                body += """
# mingw zlib installs as libzlib, but several configure scripts hardcode -lz,
# so the alias has to exist. It belongs in zlib's own directory, which is where
# the library is; v1 wrote it into the shared prefix because there was one.
if [ "$HOST_OS" = "mingw32" ]; then
  for _f in "$PREFIX/lib"/libzlib.*; do
    [ -f "$_f" ] || continue
    _ext="${_f##*libzlib}"
    [ -f "$PREFIX/lib/libz$_ext" ] || ln -s "libzlib$_ext" "$PREFIX/lib/libz$_ext"
  done
fi
"""
                REPORT.add("zlib/generic.lua", "moved the mingw libz alias into zlib's recipe",
                           "was written into the shared prefix")
            # Fixups see the shell text, with its quoting, not the TOML escaping of it.
            body = apply_fixups(name, out_name, body)
            lines.append(body_to_toml(body))

        text = "\n".join(lines).rstrip("\n") + "\n"
        with open(os.path.join(dst_dir, out_name), "w") as f:
            f.write(text)
        REPORT.add(f"{name}/{entry}", "ported", out_name)


# --- systems -------------------------------------------------------------

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


def port_system(src_dir, dst_root, native_name):
    name = rename(os.path.basename(src_dir.rstrip("/")))
    out_dir = os.path.join(dst_root, "system@" + name)
    os.makedirs(out_dir, exist_ok=True)

    fields, deps, order = parse_recipe(os.path.join(src_dir, "generic.lua"))
    fallbacks = [rename(f) for f in re.findall(r'"([^"]+)"', fields.get("recipe_fallbacks", ""))]

    buckets = {"system": [], "autoconf": [], "cmake": [], "meson": [], "cargo": [],
               "script": []}
    pending = []
    assigns = {}
    var_kind = {}
    var_comments = {}
    var_order = []

    for marker, lines in split_sections(dedent(fields.get("setup", ""))):
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
            if s.startswith(":") or re.match(r'^(if|for|while|case|unset|exit|_|eval)\b', s) or \
               s.endswith("fi") or s.endswith("done") or s.endswith("then") or s == "fi" or \
               s.startswith("_ndk") or "$(" in s and re.match(r'^[A-Za-z_]+="\$\(', s) is None:
                buckets["script"].append(line)
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
    for bs in ("autoconf", "cmake", "meson", "cargo"):
        if buckets[bs]:
            with open(os.path.join(out_dir, bs + ".toml"), "w") as f:
                f.write(f"[buildsystem.{bs}]\n" + "\n".join(buckets[bs]) + "\n")

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
    args = ap.parse_args()

    if args.mode == "systems":
        for entry in sorted(os.listdir(args.src)):
            path = os.path.join(args.src, entry)
            g = os.path.join(path, "generic.lua")
            if not os.path.isdir(path) or not os.path.isfile(g):
                continue
            if not is_system_file(open(g).read()):
                continue
            port_system(path, args.dst, args.native)
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

    REPORT.write(args.report)
    print(f"wrote {args.report}: {len(REPORT.lines)} entries", file=sys.stderr)


if __name__ == "__main__":
    main()
