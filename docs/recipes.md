# Recipes

A recipe is TOML. A package is a directory, and a build is a `sh` script — the tool never
has to understand your build system, only to hand it a consistent world.

```
libpng/
  source.toml      where the upstream sources come from
  generic.toml     the build body, for any system without its own file
  mingw64.toml     a file for one system, and only that system
  android.toml     a file for a family named in a system's fallbacks
```

Exactly one build file wins: the exact system's file, then the system's `fallbacks` in
order, then `generic.toml`. They are never merged — the same rule v1 applied to
`generic.lua` / `android.lua`, and the reason a package's file list shows up in `search`.

## A build body

The `hello` package in the tree is two files. `source.toml` fetches and lays out, and it
has no build system at all:

```toml
build = """
mkdir -p $PREFIX
cp -rf $RECIPEDIR/main.c $PREFIX
"""
```

and `generic.toml` compiles what the source node produced:

```toml
deps = ["hello@source"]

build = """
cp -rf $NESTDIR/hello@source/* .
mkdir -p $PREFIX/bin
$CC $CFLAGS main.c -o $PREFIX/bin/hello
"""
```

Two things are worth noticing. A source node is an ordinary dependency pinned to `@source`
and built like anything else, so "fetch" is not a special phase; and a build runs in a
fresh work directory under the nest, so the only files it can see are the ones a dependency
handed over — by copying, as here, or through `-I`/`-L`/`.pc` flags.

## Dependencies as data

```toml
deps = ["zlib", "libpng@mingw64", "perl@native", "own@source"]
```

A bare name inherits the recipe's own system; `@sys` picks another one; `@native` is the
default system spelled out. Every edge carries its own system and the graph node is
(package, system), so `perl@native` and `perl@mingw64` are different nodes in one graph, and
a host tool you need is an ordinary dependency pinned to `@native` — v1's implicit "the
whole native prefix is on `PATH`" is gone, so a recipe says what it uses.

Each dependency contributes `<name>_PREFIX`, and all of them together make `DEPS`; they
arrive as `-I`/`-L` flags, `PKG_CONFIG_LIBDIR` entries and `CMAKE_PREFIX_PATH` entries.
A package with a version of its own can add a variable for a dependency — the tree's
`pngprobe` carries `libpng_sys = "1.1.11"` for the version its bindings want.

## Build systems

A recipe that names one hands the driver its own fields and nothing else:

```toml
deps = ["zlib", "libpng@source"]
buildsystem = "autoconf"
```

The driver lives in the system (`system@clang-native/autoconf.toml`), and it is what sets
the compiler flags, the rpath and the install prefix. A recipe gives only what is its own:

| `buildsystem` | recipe fields |
| --- | --- |
| `autoconf` | `configure_args`, `targets`, `install_args`, `make`, `install` |
| `cmake` | `args`, `source_dir` |
| `meson` | `args`, `source_dir` |
| `make` | `targets`, `make_args`, `install_args`, `make`, `install` |
| `cargo` | `bin` (required), `features`, `no_default_features`, `install`, `env` |
| `python` | `modules`, `licenses`, `scripts`, `whole_tree` |

Real ones from the tree, one per system:

```toml
# zlib/generic.toml
deps = ["zlib@source"]
buildsystem = "cmake"
args = ["-DZLIB_BUILD_EXAMPLES=OFF"]
```

```toml
# libpng/generic.toml
deps = ["zlib", "libpng@source"]
buildsystem = "autoconf"
```

```toml
# freetype/generic.toml
deps = ["zlib", "libpng", "freetype@source"]
buildsystem = "meson"
args = ["-Ddefault_library=static", "-Dzlib=system", "-Dpng=enabled", "-Dbrotli=disabled"]
```

```toml
# pngprobe/generic.toml — a package that is a cargo crate of its own
deps = ["freetype", "libpng"]
version = "0.1.0"
buildsystem = "cargo"
env = "PNG_CONFIG=false"
bin = "pngprobe"
```

`source_dir` is for the builds that unpack somewhere other than the top of the work
directory. `make = false` (or `install = false`) is how a recipe says the configure step
already built or installed everything, which several packages need.

## Fields, and what a body sees

`version` and any other scalar a recipe carries become shell variables, so a body can use
`$version` — v1's rule, kept on purpose. A build-system recipe does not get them under
those names, because its driver would confuse them with its own fields; it reads them as
`$BS_<field>` (`$BS_args`, `$BS_bin`, `$BS_configure_args`, …). The environment a body
starts from is:

| variable | what it is |
| --- | --- |
| `$OUTPKG` | the entry's final path |
| `$PREFIX` | where to install now (`$DESTDIR$OUTPKG`) |
| `$DESTDIR` | the staging root, exported so installers honour it |
| `$RECIPEDIR` | the directory of the recipe that won |
| `$SYSDIR` | the system's directory |
| `$NESTDIR`, `$WORK`, `$STAGE`, `$CORES` | the nest, the work directory, the stage, the job count |
| `<dep>_PREFIX`, `DEPS`, `CPPFLAGS`, `LDFLAGS`, `PKG_CONFIG_*`, `CMAKE_PREFIX_PATH`, `PATH`, `LD_LIBRARY_PATH` | the closure |

There are also `pre` and `post` on any recipe — shell that runs before and after the body
(the tree's `zlib/mingw64.toml` uses `post` to add the `libz` symlink mingw needs).

## An override, and why it is a file

`zlib/mingw64.toml` is a whole recipe, not a patch:

```toml
deps = ["zlib@source"]
buildsystem = "cmake"
args = ["-DZLIB_BUILD_EXAMPLES=OFF"]

# mingw zlib installs as libzlib, while configure scripts hardcode -lz, so the
# alias has to exist. It belongs in zlib's own directory, which is where the
# library is. ...
post = """
for _f in "$PREFIX/lib"/libzlib.*; do
  [ -f "$_f" ] || continue
  _ext="${_f##*libzlib}"
  [ -f "$PREFIX/lib/libz$_ext" ] || ln -s "libzlib$_ext" "$PREFIX/lib/libz$_ext"
done
"""
```

Both files differ by one field and one fix-up; they are kept apart because what a build
does is a fact about a system, and a single file with tests inside would be a recipe that
lies about which system it was written for.

## After the body: staging and publication

An installer is told the final path and writes through `$DESTDIR`, so the staged tree
already names itself correctly, and the tool then hashes the tree, names it by that hash,
and publishes it. Before publishing it asks the entry to find itself: `.pc` files have
their absolute prefix rewritten to `${pcfiledir}/../..`, and everything that cannot be
written that way is counted and reported rather than kept quiet:

```
relax: 1 file(s) now find themselves, 3 still name the entry by path
```

That line is the honest part of the report — a `.la` file or a `-config` script still
names the entry by path, and the count is how a port shows what a move would break. See
[docs/store.md](store.md) for what moves and what does not.

## Where recipes come from

The tree in `retrolunar-packages` was ported from v1's Lua recipes by
`tools/lua2toml.py`, which writes both the tree and a report of everything it had to guess
about. It is a tool, not a runtime: the tree is plain TOML, hand-editable, and the port can
be run again over an updated v1 source when the two disagree.
