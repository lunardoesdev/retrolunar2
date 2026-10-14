# Your first project, step by step

The goal: a directory that holds a small program, a `retrolunar.toml` next to the program
that builds it, and a repository of extra packages beside it.

Everything here was run. The files are in this repository under
[`examples/mytool/`](../examples/mytool/), so the walkthrough can be followed by building
that instead of typing it:

```sh
$ cd examples/mytool
$ retrolunar --nest /tmp/exnest --no-default-repo build
retrolunar: mytool: building mytool@clang-native
$ /tmp/exnest/mytool@clang-native/bin/mytool
the answer is 42, from mylib
```

## The layout

```
mytool/
  retrolunar.toml        the manifest: what this directory is, and where packages come from
  src/main.c             the program, right next to it
  packages/              a repository of extra packages
    mylib/
      mylib.h            a package is a directory, and its files sit with its recipe
      mylib.c
      generic.toml       how to build it
```

Nothing in that tree is special: `packages/` is an ordinary retrolunar repository, the same
shape as the one the tool clones on first use, just smaller and local.

## The program

```c
/* src/main.c */
#include <stdio.h>
#include <mylib.h>

int main(void) {
    printf("the answer is %s\n", mylib_answer());
    return 0;
}
```

It needs a library that is *not* in the big tree — and that is the point of the local
repository: the package your program needs, written by you, next to it. Keeping the program
next to the manifest is not a special layout either: a build body reads it through
`$RECIPEDIR`, which is this directory.

## The extra package

```
packages/mylib/
  mylib.h
  mylib.c
  generic.toml
```

```c
/* packages/mylib/mylib.h */
#ifndef MYLIB_H
#define MYLIB_H
const char *mylib_answer(void);
#endif
```

```c
/* packages/mylib/mylib.c */
#include "mylib.h"
const char *mylib_answer(void) { return "42, from mylib"; }
```

```toml
# packages/mylib/generic.toml
# A package that is nothing but its own two files: the recipe and the sources sit
# together, so the build copies from $RECIPEDIR and there is nothing to fetch.
version = "0.1.0"

build = """
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp "$RECIPEDIR/mylib.h" "$PREFIX/include/"
$CC $CFLAGS -fPIC -c "$RECIPEDIR/mylib.c" -o mylib.o
$AR rcs "$PREFIX/lib/libmylib.a" mylib.o
"""
```

Four things make this tiny recipe work, and they are the same four in every recipe:

- **`generic.toml` is the build body** — a `sh` script, not a description of a build system,
  because this package has no build system. A file named after a system (`mingw64.toml`,
  `android.toml`) would answer for that system only, and `source.toml` is for fetching and
  unpacking upstream sources, which this package has none of.
- **`$PREFIX` is where to install.** The tool stages that directory and publishes it; no
  recipe ever names a real location, which is why the same recipe works in any nest.
- **`$RECIPEDIR` is the package's own directory**, where `mylib.c` lives. A build runs in a
  work directory elsewhere under the nest, so a source file is always `"$RECIPEDIR/…"` and
  never a bare relative path.
- **`$CC`, `$CFLAGS`, `$AR`** come from the system the package is built for. Nothing declares
  them here: they are what `system@clang-native` says the machine is.

## The manifest

```toml
# retrolunar.toml
# The manifest of this directory's own package. Its name is the directory name
# (`mytool`), so nothing here names it.
deps = ["mylib@clang-native"]

build = """
mkdir -p "$PREFIX/bin"
$CC $CFLAGS -I"$mylib_PREFIX/include" "$RECIPEDIR/src/main.c" \
    -o "$PREFIX/bin/mytool" -L"$mylib_PREFIX/lib" -lmylib
"""

# One repository of my own, searched first, and the tree next to this one for
# everything else. Both are relative to where the tool is run, which is here.
[repo.mine]
url = "./packages"

[repo.packages]
url = "../../../retrolunar2-packages"
```

- **`deps` is what the program needs.** A bare name inherits this manifest's system;
  `mylib@clang-native` says it explicitly, and a dependency for another system would be
  `libpng@mingw64`.
- **`build` is this package's build body**, exactly like a tree recipe's. `$RECIPEDIR` is
  this directory, so the program is built from `src/main.c` beside the manifest, and
  `$mylib_PREFIX` is the dependency — every dependency gets `<name>_PREFIX`, so a build never
  guesses where something landed.
- **`[repo.NAME]` says where packages come from.** They are searched in the order they are
  declared, so `mine` is asked first and a name it has is *this* one; `packages` is the tree
  with the rest of the world in it. A relative `url` is relative to where you run the tool,
  which for a project is its own directory.
- **The name of the package this directory is** comes from the directory name — here
  `mytool@clang-native`. The manifest does not name it, and because it is in no repository,
  `deps mytool@clang-native` cannot find it: `deps` asks repositories. A name on the command
  line only says which system to build for.

A local repository needs no `system@*` directories: the systems come from the tree you also
point at, and your repository only has to carry the packages the tree does not have.

## Build and run it

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo build
retrolunar: mytool: building mytool@clang-native
$ /tmp/exnest/mytool@clang-native/bin/mytool
the answer is 42, from mylib
```

`--no-default-repo` leaves the built-in repository out of it, so this build resolved against
exactly the two repositories the manifest names — no network, nothing implicit. The local
package was built as well, into an entry of its own:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo deps 'mylib@clang-native'
mylib@clang-native 0.1.0
$ find /tmp/exnest/store -name 'libmylib.a'
/tmp/exnest/store/5da95109c711251c72d3fb3d0337cf8d48c9770a55c68eb398aeb72d54a13b77/lib/libmylib.a
$ readlink /tmp/exnest/mylib@clang-native
/tmp/exnest/store/5da95109c711251c72d3fb3d0337cf8d48c9770a55c68eb398aeb72d54a13b77
```

`run` is the same closure as an environment, for one command:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo run sh -- -c 'echo "$mylib_PREFIX"'
skip mylib@clang-native (fresh)
/tmp/exnest/mylib@clang-native
```

`build --out DIR` installs *this* package into DIR while the dependencies stay in the nest —
something to point a test or a debugger at, with nothing else in it:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo build --out /tmp/exdist
retrolunar: mytool@clang-native is in /tmp/exdist
$ /tmp/exdist/bin/mytool
the answer is 42, from mylib
$ ls /tmp/exdist
bin
```

And what the build resolved to is recorded next to the manifest, so the next build starts
from the same tree even after the repositories move on:

```toml
# retrolunar.lock
# Written by retrolunar after a build. Not edited by hand: it says what
# the manifest resolved to, so the next build starts from the same tree
# even though the repositories have moved on.

[[repo]]
name = "mine"
url = "./packages"

[[repo]]
name = "packages"
url = "../../../retrolunar2-packages"
rev = "695df00ad521e2f04a66f9ed9797c4481992af63"

# Every node this project came out as: the package directory is
# <nest>/store/<entry>.
[entries]
"mylib@clang-native" = "5da95109c711251c72d3fb3d0337cf8d48c9770a55c68eb398aeb72d54a13b77"
```

## Changing things, and what the tool does about it

**Which files are inputs.** A package's inputs are its recipe files — the build file that
won and `source.toml` — plus its dependencies. Other files in the same directory are not
hashed. So a package that keeps its own sources next to its recipe, as `mylib` does, is not
rebuilt when one of those sources changes:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo build     # nothing changed
skip mylib@clang-native (fresh)
skip mytool@clang-native (fresh)
retrolunar: mytool: building mytool@clang-native
```

That last line is the command's banner: it says what it was asked to build, not what it
rebuilt. The `skip`/`building` lines above it are the truth. Edit `src/main.c`, build again,
and the three lines are the same — the binary in the nest is still the old one. To make a
source change an input, change the recipe; bumping `version` is the usual way, and it makes
the change visible in the entry's name as well:

```sh
$ sed -i 's/^version = "0.1.0"/version = "0.2.0"/' packages/mylib/generic.toml
$ sed -i 's/42, from mylib/43, from mylib/' packages/mylib/mylib.c
$ retrolunar --nest /tmp/exnest --no-default-repo build
... mylib builds, then mytool does ...
$ ls /tmp/exnest/store | wc -l        # was 2
4
$ /tmp/exnest/mytool@clang-native/bin/mytool
the answer still is 43, from mylib
```

A new `mylib` is a new entry, and the program is a *new* entry too, linking the new library:
nothing was overwritten, and whatever still pointed at the old `mylib` would still find it.
That is the freshness story in one experiment; [docs/store.md](store.md) has the rest (a
system file, a host probe, the tool's own version).

**Add a dependency** to `deps` and build: a new node appears in `deps`, is built into the
nest, and its flags arrive in your body as `<name>_PREFIX`.

## Two rules worth knowing early

**The first repository that has a package wins.** Declare your own repository before the tree
and a name both have is yours:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo deps 'hello@clang-native'
hello@source 9.9.9-mine           # from ./packages
hello@clang-native
```

With the tree declared first, the same command prints the tree's `hello` instead (no
version). This is how a project uses a patched version of a package without forking the
world.

**A package directory without a recipe for the system is an error, not a fall-through.** A
stub in your repository shadows the tree's package, and if it has no build file that answers
for the system, the build says so instead of quietly using the other one:

```sh
$ retrolunar --nest /tmp/exnest --no-default-repo deps 'hello@clang-native'
retrolunar: package 'hello' has no recipe for system 'clang-native'
  looked for hello/clang-native.toml, the system's fallbacks, and hello/generic.toml
```

Those two lines are the answer to "what do I write next": that file, or a name the system
falls back to.

## What next

- Put `packages/` under version control with the project: it is an ordinary repository, so it
  can be shared, and a manifest can also point at one with `url = "https://…"` and `rev` to
  pin it.
- When a package you wrote becomes useful elsewhere, move its directory into a shared tree: a
  recipe has no idea whether it lives in a project or in a repository.
- `retrolunar export --to /tmp/bundle /tmp/exdist/bin/mytool` collects the program and its
  libraries flat, for a machine that has neither the tool nor a nest.
