# Projects

Starting from nothing? [docs/tutorial.md](tutorial.md) walks one directory all the way
through: a manifest, a program next to it, and a repository of extra packages beside it.

A project is a directory with a `retrolunar.toml` in it. The manifest is a recipe — the same
keys a tree recipe has — for the package this directory *is*, plus where its packages come
from. There is no second format to learn: what you write for a project can be moved into a
tree, and what is in a tree can be built here.

```toml
version = "1"
deps = ["zlib@clang-native"]

build = """
mkdir -p "$PREFIX/bin"
$CC $CFLAGS "$RECIPEDIR/src/main.c" -o "$PREFIX/bin/hello" $LDFLAGS -lz
"""

[repo.packages]
url = "/home/satori/ond/git/retrolunar2-packages"
```

Two details that save an afternoon: `$RECIPEDIR` is the project directory (for a package in
a tree it is that package's directory), and a build runs in a work directory under the nest,
so a source file is `"$RECIPEDIR/src/main.c"` and never `src/main.c`.

## Building

```sh
$ retrolunar --nest /tmp/pjnest --no-default-repo build
retrolunar: pj: building pj@clang-native
$ /tmp/pjnest/pj@clang-native/bin/hello
```

The dependencies were resolved from the repository the manifest names, built into the nest,
and the package was built against them. `build` takes no target: the target is this
directory. A name on the command line only says which system to build for —
`retrolunar build zlib@mingw64` says "the package this directory is, for mingw64".

**A second version is a second entry.** Change something the build reads (`version`, the
build script, a dependency) and build again: the entry's name is the hash of its inputs, so
the new build cannot land on top of the old one, and the friendly name moves to the new one:

```sh
$ sed -i 's/^version = "1"/version = "2"/' retrolunar.toml
$ retrolunar --nest /tmp/pjnest --no-default-repo build
retrolunar: pj: building pj@clang-native
$ ls /tmp/pjnest/store | wc -l     # was 3, and now
4
$ readlink /tmp/pjnest/pj@clang-native
/tmp/pjnest/store/d518415384613bdeb81adc281c473a9c6e1c57077d96f18e52e82e57feb7abb7
```

That is what makes two versions of one package coexist — the old entry is still there for
whatever pointed at it, and `gc` removes it once nothing does.

## Running against the closure

`run` builds what the manifest needs, then runs one command with the closure's environment;
`shell` is the same with an interactive shell:

```sh
$ retrolunar --nest /tmp/pjnest --no-default-repo run sh -- -c 'echo "$zlib_PREFIX"; pkg-config --modversion zlib'
/tmp/pjnest/zlib@clang-native
1.3.1
```

Only what the manifest declares is in that environment. This is the answer to "build my
project against the nest" for CI — no prefix to export, nothing to remember.

## Installing out of the nest

`--out DIR` installs *this* package into DIR while its dependencies stay in the nest, so DIR
is the package and nothing else: something to point a test, a debugger or a recipe at.

```sh
$ retrolunar --nest /tmp/pjnest --no-default-repo build --out ./dist
retrolunar: pj@clang-native is in /tmp/pj/dist
$ ls dist
bin
```

DIR must be empty: publishing replaces it, and the tool will not remove something the
project put there.

## Repositories

```toml
[repo.packages]
url = "/srv/git/retrolunar-packages"   # a directory or a git URL
rev = "6f4f767caf1307855e0acdb55c3ed4ef26eab78f"   # optional: pin it
```

Repositories are searched in the order they are declared, the built-in `default` last;
`--no-default-repo` leaves it out, which is how a project can be hermetic without a network.
A `url` that is a local directory is used as it is.

What a build used lands in `retrolunar.lock`, next to the manifest:

```toml
# Written by retrolunar after a build. Not edited by hand: it says what
# the manifest resolved to, so the next build starts from the same tree
# even though the repositories have moved on.

[[repo]]
name = "packages"
url = "/home/satori/ond/git/retrolunar2-packages"
rev = "6f4f767caf1307855e0acdb55c3ed4ef26eab78f"

[entries]
"zlib@source" = "7719849ce81d2dc5e718b3c639e52be3eb2897d8ed017bd1082458a2e7194f6a"
"zlib@clang-native" = "9816121a28c4225fe188dd406e96bf742cd71553e2b8ecd8085111fa6da7f32a"
"pj@clang-native" = "d518415384613bdeb81adc281c473a9c6e1c57077d96f18e52e82e57feb7abb7"
```

The lock is a record, and the entries section is the useful part: it names the *content*
every dependency was built from, so a later build says different or says the same, and a
difference is visible instead of inferred.

## A cache, and a place to publish

```toml
[cache]
url = "https://cache.example/pub"      # or a directory
```

With a cache configured, `retrolunar cache pull` takes entries from it instead of building
them, and `retrolunar cache push` sends what this nest has. Both take targets, both accept
`--cache DIR|URL` to override the manifest, and what they move is the entry itself — see
[docs/store.md](store.md) for what an entry is and why it can be checked.

## CI, in four lines

```sh
retrolunar --no-default-repo run make check      # build the deps, run the suite
retrolunar --no-default-repo build --out ./out   # the package, alone, in ./out
retrolunar export --to ./bundle ./out/bin/tool   # everything it needs, flat
sh ./bundle/run.sh                               # if the bundle needed a launcher
```

`run` fails with the command's status, and `build`/`export` fail with their own, so a CI
script needs no parsing of output. When `export` writes a launcher it says so — `3 file(s),
0 unresolved, 0 on the launcher` — and `0 on the launcher` means the bundle's binaries find
their own libraries by `$ORIGIN`.
