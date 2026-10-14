# An example project

A directory that builds a small program from `src/main.c`, using a package of its own from
the repository in `packages/`. Both files are documented, step by step, with the output they
print, in [../../docs/tutorial.md](../../docs/tutorial.md).

```sh
$ retrolunar --nest /tmp/example-nest --no-default-repo build
retrolunar: mytool: building mytool@clang-native
$ /tmp/example-nest/mytool@clang-native/bin/mytool
the answer is 42, from mylib
```

`--no-default-repo` keeps the built-in repository out of it, so the build resolves against
exactly the two repositories the manifest names.
