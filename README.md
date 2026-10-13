# retrolunar 2

A package manager that prints POSIX `sh` scripts. Recipes and systems are TOML data; the
tool resolves a target into a dependency graph and emits an auditable script that builds
every node of that graph into its own directory under the nestdir.

See [PLAN.md](PLAN.md) for the design and the work order.

## Build

```sh
meson setup builddir
ninja -C builddir
./builddir/retrolunar --help
```

Needs a C compiler, `meson`, `ninja`. At runtime: `sh` and `flock` (util-linux).

## Layout

```
src/                 the tool
vendor/tomlc17/      vendored TOML parser (MIT)
tools/lua2toml.py    the port from v1 (Lua recipes to TOML), with its own report
tests/               the acceptance run
```

## Tests

```sh
tests/run.sh
```

It builds the tool, regenerates the ported tree from the v1 sources when they are
next to this one, adapts the native system to this host, resolves every package on
three systems, builds `pngprobe@clang-native` through four build systems (cmake,
autoconf, meson, cargo), exports a bundle and runs it, and then checks the
freshness rules with `tests/hashes.sh`: any file in a system's directory makes
every package of that system stale, an unused file in a package's own directory
changes nothing, and a change to the build file that wins rebuilds that package
and everything downstream.
