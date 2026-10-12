# retrolunar 2 — plan

Status: v1 studied (`../retrolunar`, `../retrolunar-packages`). Everything under
*What v1 is* and *Carried over* is read off the v1 sources; *Friction* is what the
code actually does today; *Agenda* is the open list, undecided until the
brainstorm fills it in.

No line of v1 has been changed by writing this file.

---

## What v1 is

One C binary with Lua 5.5 baked in. It resolves Lua recipes into a queue and
prints an ordinary POSIX `sh` script that builds software into a per-system
prefix. No daemon, no solver at runtime, no hidden build state.

- `src/main.c` (663 lines) — argument parsing, subcommand dispatch, Lua state
  setup, `DEFAULT_SYSTEM` global, packages-tree bootstrap, `-x` execution.
- `src/loader.lua` (583 lines) — overrides global `require`, defines
  `recipe()` / `system()`, keeps the build queue, and holds the whole script
  emitter (`require_script`).
- `src/embed.py` + a meson `custom_target` (`meson.build:55-60`) turn
  `loader.lua` into `loader_lua.c`, linked into the binary.
- `lua-5.5.1/` is vendored and built as a static library (`meson.build:8-46`).

Flow: `require` targets → recipe tables land in the queue in dependency order →
`require_script()` concatenates a `sh` script → `install` prints it, `generate`
writes it (`-o`) and/or runs it (`-x`).

### Subcommands

| command | does | touches |
| --- | --- | --- |
| `install` | queue a target, print the script to stdout | nothing |
| `generate` | same script; `-o FILE` (chmod 0755), `-x` runs it, `--cores N` | nest, network (fetches) |
| `deps` | print the queue: `name@sys version`, leaves first | tree read only |
| `search` | substring match over the tree: `name kind recipes` | tree read only |
| `-e chunk` / `script` | run Lua, no subcommand machinery | — |

Flags: `--packages DIR`, `--nest DIR`, `--cores N`, `-o/--output`, `-x/--execute`,
`-h/--help`. Both `--nest` and `--packages` have defaults
(`~/.cache/retrolunar/nestdir`, `~/.cache/retrolunar/packages`, the latter cloned
from `github.com/lunardoesdev/retrolunar-packages` on first use and `git pull`ed
afterwards).

### The loader

Three `require` forms, all resolving inside the packages roots
(`PKGDIR`, `RETROLUNAR_LIB`, `.`):

- `pack@sys` — `pack/sys.lua`, then the requested system's `recipe_fallbacks` in
  order, then `pack/generic.lua`. `sys` resolves to `DEFAULT_SYSTEM`
  (`clang-native` by default, `-DRETROLUNAR_DEFAULT_SYSTEM=...`), and its identity
  is the same as `pack@<DEFAULT_SYSTEM>`.
- `pack` — inherits the requiring module's system (`sys_stack`); at top level the C
  default.
- `./x`, `../x` — relative to the requiring file, then `RETROLUNAR_LIB`.

`recipe(t)` attaches `file`/`dir`/`sys`/`system` and appends to the queue;
duplicates by canonical key `pack@sys` are an error. Order is topological for
free: `require()` calls execute before the trailing `recipe()` call, so leaves
land first. A system that does not exist is a hard error naming the package that
asked for it (`loader.lua:167-174`).

Recipe fields that are plain strings (`version`, `git`, `tag`) are emitted as
shell variables, so `$version` works inside a build body.

### The generated script

Header: `set -eu`, `NESTDIR`/`PACKAGEDIR` absolutized, `export CORES=N`,
`mkdir -p` the nest, a hard requirement on `flock`, and a non-blocking
`flock -n 9` on `$NESTDIR/.retrolunar.lock` held for the whole run.

Per package, one `if fresh; then skip; else ...; fi` block:

1. Freshness = stamp `$NESTDIR/<sys>/.retrolunar-<name>` newer than the recipe
   file, the system file, and the system dir (any single `-nt` failing means
   rebuild; no stamp means build).
2. `WORK`/`OUT` as `mktemp -d` under `$NESTDIR/tmp`, `trap` cleanup, `cd "$WORK"`,
   `PREFIX="$NESTDIR/<sys>"`, `RECIPEDIR`, `SYSDIR`, all exported.
3. The system `setup` fragment verbatim (plain `VAR="..."` + `export` lines, may
   use `$PREFIX`/`$OUT`/`$SYSDIR`/`$WORK`).
4. `NATIVE_PREFIX="$NESTDIR/<DEFAULT_SYSTEM>"` goes on `PATH` / `LD_LIBRARY_PATH`,
   so host helpers and native libs are visible without touching `PKG_CONFIG_*`.
5. The build body verbatim (heredoc terminators normalized to column 0).
6. Textual rewrite of staged `.pc`, `.la` and cmake package-config files from
   `$OUT` to `$PREFIX` (`while read` + `awk`, never `sed -i`).
7. Publish only on success: `cp -rf "$OUT"/. "$NESTDIR/<sys>/"`, `touch` the
   stamp, drop the stage dirs, release the trap.

Summary block at the end prints the nest and one line per system that got
written, only when stdout is a tty.

### Recipes and systems (the other repo)

`retrolunar-packages` holds one directory per package or system at its root
(no `packages/` prefix): `<name>/source.lua`, `<name>/generic.lua`, optional
`<name>/<sys>.lua` or a family file such as `<name>/android.lua`, and
`<sys>/generic.lua` plus `$SYSDIR` companions (`*.cmake`, `*.ini`). A system
declares `recipe_fallbacks = {"android"}` so one family recipe covers all
Android targets.

Current size: 236 packages with a `source.lua`, 293 `generic.lua` files (of which
59 are systems: `{aarch64,armv7a,i686,x86_64} × android21..35`, `i686-mingw`,
`x86_64-mingw`, `clang-native`), 15 `android.lua` overrides, 474 `stage*.md`
hand-off files.

House rules, enforced by review: flags come from the system
(`$CMAKE_FLAGS`, `$AUTOCONF_CONFIGURE_FLAGS`, `$MESON_FLAGS`, `$CC`, `$CFLAGS`,
`$PREFIX`, `$OUT`, `$SYSDIR`), never hardcoded target facts; no patching upstream
sources (rewriting our own generated files under `$OUT` is allowed, with `awk`);
no emulation ever; serial builds unless `$CORES` is passed deliberately; no
checksums; no network at build time except `curl` in `source.lua`.

---

## Carried over to v2

These are the parts that are load-bearing and cheap to keep:

- **Script-as-artifact.** Reading, diffing and rerunning the build script is the
  product. v2 keeps the script as the primary output even if everything around it
  changes.
- **Recipe tree as data, separate repo.** Nothing in the tool knows package names.
- **Per-system prefix under one nest**, with `PREFIX` (search) and `OUT` (install
  target) as distinct paths, and publish-on-success so a failed recipe leaves no
  half-installed prefix.
- **System `setup` as one readable shell fragment** that defines the entire
  toolchain environment; build-system defaults live there, not in recipes.
- **The `pack@sys` / bare / relative resolution model** and dependency order by
  require order.
- **Freshness stamps plus a documented way to force a rebuild.**
- **Fail-fast nest lock** rather than queuing.
- **`deps` and `search` as tree-only, nest-free commands.**
- **The authoring rules**: system-supplied flags, no upstream patching, no
  emulation, verifiable stage evidence.

## Friction found in v1 (candidate work for v2)

1. **Freshness is one `-nt` chain.** Editing anything in a system dir rebuilds
   every package for that system; a recipe that changes behaviour without changing
   its file (an upstream version only reachable through a dep) is invisible. No
   content hash, no dependency-version input, no record of what a build produced.
2. **No dependency data.** Build order is recipe text order. There are no version
   constraints, no conflict detection, no automatic `@source` requirement — a
   recipe that copies from `$NESTDIR/source/<name>` silently breaks if the author
   forgets `require("own@source")`.
3. **The emitter is hand-written string concatenation** (~190 lines of
   `loader.lua:364-582`) with hand-rolled shell quoting (`sh_sq`). It has no tests:
   the repo has no test suite at all, and the emitter is the thing most likely to
   be silently wrong.
4. **`--cores` defaults to 1 and recipes must opt in.** Deliberate, but there is
   no per-package job policy, and `MAKEFLAGS=-j$CORES` in the systems fights the
   "a bare `make` is fine" rule.
5. **Lua has no `readdir`.** `search` shells out to `ls -1` through
   `io.popen`; `abspath` shells out to `pwd`; `main.c` uses `system()` for
   `git clone`/`git pull`. Pieces of the tool depend on the host shell quietly.
6. **Packages tree is a floating `git clone --depth=1` in a shared cache dir**,
   refreshed with `git pull --ff-only` on every run, with no lock and no pinning —
   two concurrent runs and a moving upstream are both unmodelled.
7. **One prefix per system, merged in place.** Nothing records which files a
   package installed, so removed files linger, install order is not reversible,
   and there is no uninstall or `list` command.
8. **`$OUT`→`$PREFIX` rewriting is textual and directory-limited** (a fixed glob
   list of `lib/pkgconfig`, `share/pkgconfig`, `lib/*.la`, `lib/cmake/**`,
   `share/cmake/**`). Anything outside it keeps a stale staging path.
9. **59 hand-copied system directories.** AGENTS.md's rule is literally "new API
   level = copy the whole dir, rename every `NN`". The same NDK logic is repeated
   per arch and per level; a fix has to be applied 4× or 8×.
10. **No download cache outside the nest.** `nest/source` is per-nest, so CI and
    every local nest refetches tarballs.
11. **Flag parsing is duplicated three times** in `main.c` (`do_install`,
    `do_generate`, `do_deps`/`do_search`) plus a pre-scan in `main`, kept in sync by
    the hand-maintained `VALUE_FLAGS`/`BOOL_FLAGS` lists.
12. **No distribution story.** The binary must be built with meson+ninja+python3
    before anything works; there is no released artifact and no self-check.
13. **Diagnostics end at the recipe.** A failed build is a shell exit status and a
    log; nothing maps a failure back to the recipe line or to the stage files.

## Agenda for the brainstorm

Everything here is undecided. Grouped so we can strike whole groups:

- **Scope of v2**: a breaking rewrite of the tool, an incremental path, or a
  second tool that consumes the same recipes?
- **Recipe format**: keep Lua (`require`-as-dependency, string build bodies),
  or a declarative format the tool can inspect statically?
- **Dependency model**: versions, constraints, optional features, conflicts,
  `@source` as an implicit edge, cross-system dependencies.
- **Freshness and state**: content hashes vs stamps, a manifest of installed
  files, what invalidates what, and how a rebuild stays honest.
- **Install model**: per-package prefix + generation/symlink switch, or the
  current merged prefix; uninstall and `list`.
- **Systems and toolchains**: keep 59 hand-copied dirs, generate them per API
  level, or parameterize arch × level in one file; host-native and non-Android
  targets beyond mingw.
- **Concurrency**: safe parallel package builds, per-package locks, a real
  scheduler, and how `$CORES` relates to it.
- **Caching**: shared download cache, shared native-prefix cache, CI reuse.
- **UX**: command set, machine-readable output (`deps --json`), what
  `install`/`generate` should be called, script-only vs tool-driven builds.
- **Packaging**: a released self-contained binary, bootstrap path, versions of the
  tool itself.
- **Testing**: emitter golden tests, recipe linting, a smoke build per system.
- **The stage1/2/3 pipeline**: does it survive v2, and does the tool help enforce
  it (a `check` command over the stage files)?
