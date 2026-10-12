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
```
