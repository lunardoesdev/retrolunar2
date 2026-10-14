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

Needs a C compiler, `meson`, `ninja`. At runtime: `sh`, `flock` (util-linux), `awk` (it is what
lets an entry name itself by its own location), `tar`, `git` when a repository is fetched,
`curl` when the cache is remote.

## Commands

```
deps, search, install, generate, env   resolve and emit
export     a bundle of a target and its libraries, to hand to a machine with no nest
shell      build what a directory's retrolunar.toml needs, then a shell in it
run CMD    the same environment, one command (CI)
build      the package this directory is: into the nest, or into --out DIR
gc         remove store entries nothing points at
cache      push built packages to a cache, or pull them instead of building
```

A project's `retrolunar.toml` is a recipe — the same keys a tree recipe has — and
it may name where its packages come from:

```toml
deps = ["zlib@clang-native"]

[repo.mine]
url = "/srv/git/retrolunar-packages"   # or https://…, with rev to pin it

[cache]
url = "https://cache.example/pub"
```

Repositories are searched in the order declared, the built-in `default` last;
`--no-default-repo` leaves it out. What a build used lands in `retrolunar.lock`.

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
and everything downstream. It then builds a project of its own: its manifest, its
repository, its environment, `build --out`, a second version of the same package
keeping its own store entry, `gc`, and the cache — an entry built in one nest and
run from another, and a tampered entry refused.
