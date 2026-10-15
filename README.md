# retrolunar 2

A package manager that prints POSIX `sh` scripts. Recipes and systems are TOML data; the
tool resolves a target into a dependency graph and emits an auditable script that builds
every node of that graph into its own directory under the nestdir.

Nothing is built behind your back: the script *is* the build. Read it, keep it, run it by
hand, archive it. Everything the build needs is decided before it starts, and every
package lands in a directory of its own, so no package can quietly read another one's
files — what it can read is what came through its dependencies.

Version 2.10.0. [PLAN.md](PLAN.md) is the design, the reasoning and the record of the work;
this README and `docs/` are how to use the thing.

| | |
| --- | --- |
| [docs/tutorial.md](docs/tutorial.md) | your first project: a manifest, a program beside it, a repository of your own packages |
| [docs/tool.md](docs/tool.md) | the commands, the script they print, where things land |
| [docs/recipes.md](docs/recipes.md) | writing a package: a build body, a build system, an override |
| [docs/systems.md](docs/systems.md) | what a system is, and how to add one |
| [docs/projects.md](docs/projects.md) | a project's manifest, its repositories, its lock, CI |
| [docs/store.md](docs/store.md) | entries, freshness, the cache, and moving a nest |

## Build

```sh
meson setup builddir
ninja -C builddir
./builddir/retrolunar --help
```

Needs a C compiler, `meson`, `ninja`. At runtime: `sh`, `flock` (util-linux), `awk` (it is
what lets an entry name itself by its own location), `tar`, `sha256sum`, `git` when a
repository is fetched, `curl` when the cache is remote.

## Quick start

A tree of recipes is needed; the built-in one is cloned on first use, and a local one can
be named instead.

```sh
$ ./builddir/retrolunar --packages ../retrolunar2-packages deps 'libpng@clang-native'
zlib@source 1.3.1
zlib@clang-native
libpng@source 1.6.48
libpng@clang-native
```

`deps` prints the graph, leaves first: that is the order the script builds in. Then build
it — `generate` prints the script, `-o FILE` keeps it, `-x` runs it:

```sh
$ ./builddir/retrolunar --packages ../retrolunar2-packages --nest /tmp/nest generate -x 'libpng@clang-native'
... four packages, four build systems ...
retrolunar: /tmp/nest/libpng@clang-native: an entry built here
relax: 1 file(s) now find themselves, 3 still name the entry by path

$ ./builddir/retrolunar --packages ../retrolunar2-packages --nest /tmp/nest generate -x 'libpng@clang-native'
skip zlib@source (fresh)
skip zlib@clang-native (fresh)
skip libpng@source (fresh)
skip libpng@clang-native (fresh)
```

The second run builds nothing, because nothing that any of those packages depends on has
changed. Each package is in a directory named by its own content, and a friendly name
points at it:

```sh
$ ls -l /tmp/nest/libpng@clang-native
libpng@clang-native -> /tmp/nest/store/304f73019faa8ab485a3d7457d33713d25ead419dbefff3db6986ce6314dfb46
```

To work against what you built, take its environment. There is no shared prefix to point
at, so the environment is compiled from the closure you asked for:

```sh
$ eval "$(./builddir/retrolunar --packages ../retrolunar2-packages --nest /tmp/nest env 'libpng@clang-native')"
$ echo "$libpng_PREFIX"
/tmp/nest/libpng@clang-native
$ pkg-config --modversion libpng
1.6.48
```

`env` is the answer to "how do I build against the nest", and it is the same closure→env
code the generated script uses. Inside a project, `run` and `shell` do it for you.

## Layout

```
src/                 the tool
vendor/tomlc17/      vendored TOML parser (MIT)
tools/lua2toml.py    the port from v1 (Lua recipes to TOML), with its own report
docs/                how to use it
```
