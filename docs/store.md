# Entries, freshness, the cache

## An entry

Everything a build produces goes into a directory named by the hash of everything that went
into it:

```
nestdir/store/9816121a28c4225fe188dd406e96bf742cd71553e2b8ecd8085111fa6da7f32a/
nestdir/zlib@clang-native -> nestdir/store/9816121a…
```

The hash covers the recipe files that won (the build file and `source.toml`, since a version
move there must rebuild), the fingerprint of the system it builds under, the fingerprints of
its dependencies *recursively*, and the tool's own version. Editing any file in a system's
directory, or any dependency's, or the tool itself, changes the name — so a different input
cannot overwrite an entry, and two versions of one package coexist by construction.

A name (`zlib@clang-native`) is a symlink to whichever entry is current for that node. It is
written as an absolute path, so it is a convenience *of this nest*: what travels is the
store — the entries — and a name is written again the next time that node is built or
pulled.

## State, and what makes a package stale

```
nestdir/.retrolunar/state/clang-native/zlib.toml
    fingerprint = "9816121a…"
    probes      = "cffaf4081653aa24d7b9fb934e95b632483a9d5aea43853fb62c63195c1e67c0"
    tool        = "2.10.0"
```

The file lives outside the entry on purpose: an entry stays a pure artifact, and nothing
inside it has to be rewritten to record a build. A package is rebuilt when

- its recipe changes, or a dependency's fingerprint changes (the cascade agreed: a changed
  dependency rebuilds the dependent, with no "it rebuilt to the same bytes" refinement);
- any file in its system's directory changes, by name or content;
- a host fact a `probe` reports changes;
- **the tool itself changes** — the version is an input of every fingerprint, and a state
  file names the tool that wrote it, so a state written by another tool is not an answer
  about what this one would build. An upgraded tool rebuilds; it does not inherit.

There is one shortcut, and it is timestamps: when nothing that feeds a node is newer than
its state file, the stored fingerprint is taken as it is instead of hashing the inputs
again. A restored timestamp can slip past it, which is what `RETROLUNAR_VERIFY=1` is for.

## gc

```sh
$ retrolunar --nest /tmp/nest gc
retrolunar: 2 entries removed, 4 in use
```

An entry nothing points at is a rebuild nobody asked for; `gc` removes those, and only
those. Nothing in the store is ever overwritten or edited, so a `gc` that removes the wrong
thing costs a rebuild, not a broken build.

## The cache

```sh
$ retrolunar --nest /tmp/nest cache push --cache /tmp/cache 'zlib@clang-native'
retrolunar: /tmp/nest: 2 entries sent, 0 already here, 0 not in the cache
$ ls /tmp/cache
7719849ce81d2dc5e718b3c639e52be3eb2897d8ed017bd1082458a2e7194f6a.sha256
7719849ce81d2dc5e718b3c639e52be3eb2897d8ed017bd1082458a2e7194f6a.tar.gz
9816121a28c4225fe188dd406e96bf742cd71553e2b8ecd8085111fa6da7f32a.sha256
...
$ retrolunar --packages ../retrolunar2-packages --nest /tmp/other cache pull --cache /tmp/cache 'zlib@clang-native'
retrolunar: /tmp/other: 2 entries fetched, 0 already here, 0 not in the cache
```

`pull` takes entries instead of building them, and each one is checked against its name
before it is accepted: the directory is *named* by the hash of its contents, so a tampered
entry does not match what it claims to be, and the tool refuses it. The cache can be a
directory or an HTTP(S) URL, and `--cache` beats `[cache] url` in the manifest.

The direction this is going is written in [PLAN.md](../PLAN.md): a nest *is* a cache, since
an entry is already a directory named by its content, and there is nothing to pack or
unpack when you can point at one. `push`/`pull` is today's way of moving entries between
machines that cannot see each other's nest.

## An entry names itself

Anything an entry says about where it was built is a path that has to survive the entry
being somewhere else. Two of the three cases are handled by rewriting at publication:

- **`.pc` files** are rewritten to name themselves by their own location —
  `${pcfiledir}/../..` — so `pkg-config` answers correctly wherever the entry is. The
  acceptance proves it by moving the whole nest aside and asking again:

```
zlib's .pc answered /…/nest/store/a8f2f878…/share/pkgconfig/../.., and
/…/moved/store/a8f2f878…/share/pkgconfig/../.. after the whole nest moved away
```

- **ELF runtime paths** carry `$ORIGIN`, `$ORIGIN/../lib` and `$ORIGIN/../lib64`, so a
  program finds its own entry's libraries after a move — and, just as important, does not
  silently take a host library of the same soname instead:

```
and pngfix found /…/moved/store/304f7301…/bin/../lib/libpng16.so.16
```

- **what is left is reported, not hidden.** A libtool `.la` file, a `-config` script and a
  path compiled into a binary have no relative form, so `relax` counts them:

```
relax: 1 file(s) now find themselves, 3 still name the entry by path
```

That line is the honest state of a port: those are the files that would have to be inspected
if an entry were moved, and the count is how you find them. Everything else about an entry
is content, and content moves.

## Moving a store, and handing one over

A store is a directory tree of entries, so copying it to a flash drive is copying
directories — no archives, no links to recreate, no permissions to fix up. What you get is
every package, each named by its inputs, and the tool can be pointed at it with `--nest`.

For a machine with neither the tool nor the tree, `export` writes a bundle instead:

```sh
$ retrolunar --nest /tmp/nest export --to /tmp/bundle 'pngprobe@clang-native'
retrolunar: bundle: 3 file(s), 0 unresolved, 0 on the launcher
$ ls /tmp/bundle
libpng16.so.16
libpng16.so.16.48.0
libz.so.1
libz.so.1.3.1
pngprobe
```

Flat, because a bundle is not a nest: the binaries and the libraries they load, deduplicated
by soname so a versioned chain comes along whole, with `$ORIGIN` baked in so they find each
other. Anything that cannot be resolved is listed, and — unlike in a nest, where a missing
library is a later surprise — an unresolved name is an error: an export that is quietly
broken is worse than one that refuses. A file that carries no runtime path at all (a static
binary, or a cargo binary built by a system that does not set `$RUSTFLAGS`) makes the bundle
say `N on the launcher` and leaves a `run.sh` beside it, which sets `LD_LIBRARY_PATH` to the
bundle's own directory.
