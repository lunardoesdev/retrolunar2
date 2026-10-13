# retrolunar 2

A package manager that prints POSIX `sh` scripts. Recipes and systems are TOML data; the
tool resolves a target into a dependency graph and emits an auditable script that builds
every node of that graph into its own directory under the nestdir.

v1 (`../retrolunar`, the Lua-baked binary) stays frozen and is the porting source;
recipes come over from `../retrolunar-packages`.

## Status

P0–P5 are done. The tool, the port and the acceptance run all hold on the host rig; Android is
the one thing not built (deliberately last, and not verifiable on this machine).

The port (`tools/lua2toml.py`, `../retrolunar2-packages`) converted 59 systems and 236 packages
to TOML, and `PORTING.md` records every line it dropped, rewrote or fixed by hand — the
`$PREFIX` of v1 was a single shared directory, and each of its uses had to name something
specific instead. All 781 emitted files parse with both this tool's reader and `tomllib`.

Acceptance, run end to end after the last change:

- 711 combinations of package × system resolve with no errors.
- A clean native build of `pngprobe@clang-native` (zlib → libpng → freetype → pngprobe, the last
  through cargo) takes 87 s; the run right after it skips 7 of 7 nodes as fresh.
- A new file inside `system@clang-native/` — not a TOML file, any file — makes the four build
  nodes of that system stale and leaves the three source nodes alone; taking it away brings the
  tree back to 7 of 7 fresh. An unused file inside a package directory rebuilds nothing;
  changing the build file that wins rebuilds it and everything downstream.
- `export` collects the runtime closure by soname (`libpng16.so.16`, `libz.so.1`), leaves the
  host's own libraries out, and rewrites an existing runtime path to `$ORIGIN` when the string
  fits. Files with no runtime path at all — cargo binaries among them — are left untouched and
  the bundle carries `run.sh`, which is what makes it work on a flash drive.

What is left: Android (one system, one package chain), and giving any ELF a runtime path by
adding a segment (`export` cannot do it today without risking the file, see the notes in
`src/bin.c`).

## Shape of the thing

- One C binary. Lua is gone, TOML replaces it; v1's `loader.lua` and `main.c` are the
  reference for behaviour, not for structure.
- The output is still a plain `sh` script you can read, diff, edit and rerun.
- Every package gets its own directory in the nestdir, so nothing may assume a shared
  prefix holds all dependencies.
- Freshness is content hashes plus optional environment probes, never mtimes.

## Build systems

A recipe names the build system it wants and gives only what is its own business:

```toml
# zlib/generic.toml
deps = ["zlib@source"]
buildsystem = "cmake"
# Only the library is used; the examples cannot run on the build host.
args = ["-DZLIB_BUILD_EXAMPLES=OFF"]
```

`cmake`, `autoconf`, `meson`, `make`, `cargo` and `xmake` exist, plus the shell body a
recipe can carry instead. A package's fields become `$BS_*` variables — `args`,
`source_dir`, `features`, `bin`, whatever the script asks for — and a recipe's `pre` and
`post` are shell that runs around the standard steps, which is where its own preparation
belongs.

The commands themselves are data too: `buildsystem = "cmake"` means "use the `script` in
this system's `[buildsystem.cmake]`", and nothing in the tool knows what cmake is. That is
also why a build system lives in a system and nowhere else: the fingerprint of the system
directory then covers the driver, and a reader sees the whole script without following any
inheritance.

## Formats

Package directory:

```
zlib/
  source.toml      where the upstream sources come from
  generic.toml     fallback build body
  mingw64.toml     optional: wins instead of generic.toml for one system
  android.toml     optional: wins for a family named in a system's fallbacks
```

Exactly one build file wins: the exact system, then the system's `fallbacks` in order,
then `generic.toml`. Fallback and override are not merged — the same rule v1 applied to
`generic.lua` / `android.lua`.

System directory, named `system@<name>` (`system@clang-native`, `system@mingw64`,
`system@aarch64-android24`):

```
system@clang-native/
  system.toml     identity: os, arch, triplets, fallbacks, probes
  autoconf.toml   [buildsystem.autoconf] -> its settings and its script
  cmake.toml      [buildsystem.cmake]    -> $CMAKE_FLAGS, toolchain file, script
  meson.toml      [buildsystem.meson]    -> $MESON_FLAGS, cross file, script
  cargo.toml      cargo's cross variables and its script
  make.toml       what a plain make build looks like here
  *.cmake, *.ini  companion files referenced by name
```

Peer files in a system directory merge into one namespace, and the same key in two of them
is an error, never a silent override. `[system]` is the symbol table every other fragment
reads through `${system.<key>}`; an unknown symbol fails at generate time instead of
producing a build that quietly installs nothing.

Build bodies stay shell (`build = """..."""`), as in v1. `[buildsystem.*]` fragments only
define variables, with v1's names, so the tool never has to tell cmake from meson — and
porting a recipe is mostly copying its body. Arbitrary computation is allowed and happens
in the generated script (NDK discovery by glob, `command -v make`), exactly like v1's
`setup` fragment: the tool validates structure, not what the shell computes.

## Naming and CLI

- Long form only in directory names. CLI and `deps` use short names: `zlib@mingw64`.
- `@native` is the alias of the compile-time default system, same identity as
  `zlib@<DEFAULT_SYSTEM>`, and `deps` prints it expanded.
- A name lives in the directory name and nowhere else: values use the short form
  (`host = "clang-native"`), package overrides are `mingw64.toml` / `android.toml`.

## Dependencies as data

```toml
deps = ["zlib", "libpng@mingw64", "perl@native", "own@source"]
```

Every edge carries its own system and the graph node is (package, system), so `perl@native`
and `perl@mingw64` are different nodes. The tool topologically sorts and detects cycles;
build order is no longer recipe text order, and a forgotten `own@source` becomes visible
instead of breaking a later stage silently. There is no version constraint solving and no
conflict resolution: one version per package, moved by editing `source.toml`.

## nestdir

`nestdir/<pack>@<sys>/`, one directory per package, Nix-style. No shared prefix exists, so
the environment is compiled from each node's closure:

| variable | built from |
| --- | --- |
| `PKG_CONFIG_PATH`, `PKG_CONFIG_LIBDIR` | each dependency's `lib/pkgconfig`, `share/pkgconfig` |
| `CMAKE_PREFIX_PATH` | each dependency's directory (cmake config files live there) |
| `CPPFLAGS`, `LDFLAGS` | `-I<dep>/include`, `-L<dep>/lib`, for packages shipping no `.pc` |
| `PATH`, `LD_LIBRARY_PATH` | **native** dependencies only — a cross toolchain never goes on `PATH` |

Host tools arrive through ordinary `deps` entries pinned to `@native`. v1's implicit "the
whole native prefix is on `PATH`" is gone, so every ported recipe is rechecked for the host
tools it relied on without saying so.

Installation uses the final `--prefix` plus `DESTDIR=$STAGE`, then the staged tree is
renamed into place. `.pc` files, cmake modules and `*-config` scripts then carry the final
path from the start, and v1's `awk` rewrite of `$OUT` → `$PREFIX` disappears entirely.
cargo does not fit this shape and stays behind the script escape hatch.

## Caching and invalidation

sha256 implemented in-repo, no libcrypto.

- **System fingerprint**: every file in the system directory (names plus content), the
  system name, and the tool's own version. Editing any file in a system directory makes
  every package of that system stale — that is the point of the exercise, and it is
  stronger than v1, whose directory-mtime comparison missed in-place edits.
- **Package fingerprint**, per node: the recipe files, `source.toml`, its own system
  fingerprint, and its dependencies' fingerprints recursively. Cascade agreed: a changed
  dependency rebuilds the dependent, with no "it rebuilt to the same bytes" refinement.
- **State** lives in `nestdir/.retrolunar/state/<sys>/<pkg>.toml`, not inside package
  directories, so a package directory stays a pure artifact.
- **Probes** make host changes visible:

  ```toml
  [system]
  probe = ["${cc} --version", "cmake --version"]
  ```

  They run in the script *after* the system setup, so values the setup computed (`$NDK`)
  are available. Results are hashed (output plus exit status) into state, and a change marks
  the system's packages stale. No `probe` declared means the freshness check needs no setup
  run at all — file hashes only, v1 behaviour. A non-zero probe status is a change, not a
  build failure.

## Commands

- `install` — the script, to stdout.
- `generate` — the same script, with `-o FILE`, `-x` to run it, `--cores N`.
- `deps` — the resolved graph, leaves first, versions included.
- `search` — what is in the tree.
- `env <target>` — the closure's environment as `export` lines, from the same closure→env
  code the script uses: `eval "$(retrolunar env libpng@native)"`. This is the answer to
  "how do I build against nestdir" once there is no single prefix.
- `export --to DIR <target | path>` — runtime shipping, not an SDK: the target's binaries
  and shared libraries plus what they actually load, flat, ready for a flash drive. Walk
  `NEEDED` (ELF) and the PE import table recursively, resolve by soname so versioned
  symlink chains come along, dedupe. Parsed by the tool itself, so no binutils is required.
  Unresolved names are classified: present in nestdir → copied; provided by the platform or
  toolchain (bionic `libc`/`libm`/`libdl`, Windows API, `libgcc_s*`) → ignored; anything
  else → listed, and only `--strict` makes it an error. ELF bundles get an `$ORIGIN` rpath
  so they find their own libraries (`--no-rpath` turns it off).

## The package block (the P2 contract)

Every block gets this prelude from the emitter, and the system fragment and the recipe body
are written against it:

| variable | meaning |
| --- | --- |
| `NESTDIR` | the nest root; every package directory lives directly under it |
| `OUTPKG` | this package's final directory, `$NESTDIR/<pack>@<sys>` — what `--prefix` and `-DCMAKE_INSTALL_PREFIX` get, because config files must record the final path |
| `STAGE` | scratch staging tree, exported as `DESTDIR` |
| `PREFIX` | `$DESTDIR$OUTPKG` — where a hand-written install writes right now |
| `DEPS` | the dependency directories, in topological order |
| `WORK`, `RECIPEDIR`, `SYSDIR`, `PACKAGEDIR`, `CORES` | as in v1 |
| `CPPFLAGS`, `LDFLAGS`, `PKG_CONFIG_PATH`, `PKG_CONFIG_LIBDIR`, `CMAKE_PREFIX_PATH`, `PATH`, `LD_LIBRARY_PATH` | built from the closure: `-I<dep>/include` and `-L<dep>/lib` per dependency, each dependency's `lib/pkgconfig` and `share/pkgconfig`, each dependency's directory for cmake, and `bin`/`lib` of the **native** dependencies only |

Because `DESTDIR` is exported for the whole block, a recipe still says `make install`,
`cmake --install build` or `meson install` — the tooling redirects it into the staging tree
by itself, and the staging tree is put in place only on success. That is what lets a ported
recipe body stay as it was in v1.

The emitter then appends the system's own variables, so a fragment can extend what the
closure produced: `CPPFLAGS = "$CPPFLAGS -isystem $SYSROOT/usr/include"` works because the
closure's value is already in the environment by then.

## Work order

- **P0** scaffolding: meson project, vendored tomlc17 plus a typed TOML reader of our own
  that rejects unknown keys with a real message, CLI dispatch, `--help`.
- **P1** model and resolver: system directory merge, package file selection, the dependency
  graph, topological sort, cycle detection, `deps`.
- **P2** environment compiler: closure to environment, per-package directories, `DESTDIR`
  staging, publish, nest lock, script emitter.
- **P3** state: sha256, file fingerprints, probes, skip/rebuild, `--force`, `search`.
- **P4** test rig on the host toolchain: `system@clang-native` (clang, meson, cmake, ninja)
  plus `m4` (configure), `zlib` (cmake), `fribidi` (meson), `libpng` (cmake, needs zlib) and
  a `pngprobe`-style artifact that links libpng, to prove dependencies resolve across
  separate directories.
- **P5** `env`, `export`, then porting systems and packages from `retrolunar-packages`.
- **P6** Android and the other cross targets last.

Acceptance for P4: clean build; rerun prints `skip`; editing
`system@clang-native/cmake-toolchain.cmake` makes everything stale; the probe links libpng
out of `nestdir/libpng@clang-native`.

## Still open

- Array handling when peer TOML files merge (assumption: replace, not append).
- Whether `env` needs a `--json` form.

## Planned: what is agreed and not built yet

Features he asked for, in the order they will be built. Each phase ends with the
acceptance green, and `[repo.*]`/`[cache]` appear in the manifest only in the
phase that reads them.

1. **The manifest and the local shell.** `retrolunar.toml` in a directory is a
   recipe: the same keys as a tree recipe (`deps`, `buildsystem`, `args`,
   `pre`/`post`), no `version`, plus `[shell] deps` for what only the shell needs.
   The name is the directory's. `deps` is shared: the package needs it, the shell
   provides it. Commands: `shell` (build what is missing, then a shell with the
   closure's environment), `run <cmd>` (the same without the interactive shell,
   for CI), `build [--out DIR]` (the package itself, installed into DIR).
2. **A store addressed by content.** A build's key is the fingerprint that already
   exists (recipe text, system fragments, dependencies transitively, probe
   output), so entries live in `store/<hash>` and two revisions of a repository
   cannot overwrite each other, while the same package in two projects is one
   entry. `nest/<pkg>@<sys>` becomes a friendly name pointing at the entry the
   current lock chose; unused entries are collected on demand.
3. **Repositories.** A list, resolved in the order the manifest declares them,
   with the built-in `default` last so a project's repository wins. `--repo
   NAME=URL[@REV]` adds one for a single run; `--no-default-repo` drops the
   built-in one, which is how a build proves it does not quietly depend on it.
   The lock file records the order, the revisions and each package's key.
4. **A binary cache, local and remote.** Local comes free from phase 2; remote is
   the same entry over HTTP, pulled before a build and pushed after, verified
   against a recorded content hash. Tokens come from the environment, never from
   a file in the project.
