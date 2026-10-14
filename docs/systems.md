# Systems

A system is everything a package needs to know about where it is being built: the
toolchain, the triplet, the flags, and the driver for each build system. It is a directory
named `system@<name>`, and a build never sees a system through a variable it did not get
from one.

```
system@clang-native/
  system.toml     identity: the toolchain, the triplets, the probes
  autoconf.toml   [buildsystem.autoconf] -> its settings and its script
  cmake.toml      [buildsystem.cmake]    -> $CMAKE_FLAGS, toolchain file, script
  meson.toml      [buildsystem.meson]    -> $MESON_FLAGS, cross file, script
  cargo.toml      cargo's cross variables and its script
  make.toml       what a plain make build looks like here
  python.toml     where site-packages is, and how modules are placed
  *.cmake, *.ini  companion files referenced by name
```

The tree ships 57 of them: `clang-native`, `mingw32`, `mingw64`, and `android` at fourteen
API levels for each of `aarch64`, `armv7a`, `i686`, `x86_64`.

## Identity

`system.toml` says what the machine is. From `system@clang-native`:

```toml
[system]

CC = "clang"
CXX = "clang++"
AR = "ar"
RANLIB = "ranlib"
LD = "ld"
AS = "$CC"
STRIP = "strip"
OBJCOPY = "objcopy"
READELF = "readelf"
OBJDUMP = "objdump"
# $PREFIX points at this system's nest dir, where deps landed.
# CPPFLAGS/LDFLAGS cover the autoconf probes (e.g. libpng's
# zlib check); plain CFLAGS stay clean for direct $CC users.
CFLAGS = "-O2 -fPIC $CPPFLAGS"
CXXFLAGS = "-O2 -fPIC $CPPFLAGS"
LDFLAGS = "$LDFLAGS -Wl,--undefined-version"
# Ignore host .pc files: only ours count.
PKG_CONFIG_PATH = "$PKG_CONFIG_PATH"
BUILD_TRIPLET = "x86_64-pc-linux-gnu"
HOST_TRIPLET = "$BUILD_TRIPLET"
TARGET_TRIPLET = "$HOST_TRIPLET"
HOST_ARCH = "x86_64"
HOST_OS = "linux"
# MAKEFLAGS covers bare `make`, including recursive submakes.
MAKEFLAGS = "-j$CORES"
probe = ["clang --version", "meson --version", "cmake --version", "pkg-config --version"]
```

Three things in there are worth the words:

- **`$CFLAGS` stays clean.** A recipe that runs `$CC` directly should not get the flags
  that exist for autoconf's probes, so those live in `$CPPFLAGS` (and in the flavour of
  `CFLAGS` the system hands to build systems).
- **`HOST_ARCH` / `HOST_OS`** exist because some configure scripts are not autoconf and
  reject `--host`/`--build`; they read the arch and OS instead, spelled their own way.
- **`probe`** makes host facts visible. The commands run in the script after the system's
  setup, their output and exit status are hashed into state, and a change marks every
  package of that system stale. A non-zero status is a *change*, not a build failure — a
  compiler that stopped working should rebuild the world, not stop the build with a
  message about probes. With no `probe` written, freshness is file hashes only.

## Build-system fragments

Each fragment defines variables and one `script`, and the script is what a recipe with
`buildsystem = "..."` runs. From `autoconf.toml`:

```toml
[buildsystem.autoconf]
AUTOCONF_CONFIGURE_FLAGS = "--build=$BUILD_TRIPLET --prefix=$OUTPKG"
script = """
LDFLAGS="$LDFLAGS -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,'$$ORIGIN/../lib64'"
./configure $AUTOCONF_CONFIGURE_FLAGS $BS_configure_args
...
"""
```

and from `cargo.toml`:

```toml
[buildsystem.cargo]
CARGO_BUILD_TARGET = "x86_64-unknown-linux-gnu"
script = """
RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=-Wl,-rpath,\$ORIGIN -C link-arg=-Wl,-rpath,\$ORIGIN/../lib"
export RUSTFLAGS
${BS_env:+env $BS_env }cargo build --release --target "$CARGO_BUILD_TARGET" -j "$CORES" ...
"""
```

The spelling of `$ORIGIN` belongs to the driver, not to the system file that happens to
hold it: `make` reads `$ORIGIN` as `$O` followed by `RIGIN` unless it is written `$$ORIGIN`
inside shell-level quotes, while `cmake` and `meson` hand the value to the compiler
directly. The three entries are `$ORIGIN`, `$ORIGIN/../lib` and `$ORIGIN/../lib64` because
a program lands in `bin/` and its libraries in `lib/`, and `$ORIGIN` alone would be the
`bin` directory — where the loader finds nothing and takes a host library of the same
soname instead.

`$OUTPKG` is the final path and `$BS_*` are the recipe's fields; a driver that ignores
either is a driver that installs into the wrong directory or the wrong place.

## The rules

- **Peer files merge into one namespace, and the same key twice is an error**, never a
  silent override. Two fragments both defining `CC` is a tree that has to be fixed.
- **`[system]` is the symbol table** every fragment reads through, and `${system.<key>}`
  is how a build-system fragment refers to a fact about the system. An unknown symbol
  fails at generate time, rather than producing a build that quietly installs nothing.
- **A system's script may compute.** Finding the NDK by glob, asking `command -v make`,
  working out a triple — that is shell, and it runs in the generated script, exactly where
  v1's `setup` fragment ran. The tool validates structure, not what the shell computes.
- **Any file in the system's directory is part of its fingerprint** — TOML or not, by name
  and content — so editing anything here makes every package of that system stale. That is
  the point of the exercise: a system is a build input and cannot change silently.

## Adding a system

Copy the closest one and change what is different. For a cross compiler:

```sh
$ cp -r system@clang-native system@aarch64-mydevice
$ $EDITOR system@aarch64-mydevice/system.toml
```

```toml
[system]
fallbacks = ["mydevice", "android"]     # recipe files that may answer for this system
CC = "$TOOLBIN/aarch64-mydevice-clang"
CXX = "$TOOLBIN/aarch64-mydevice-clang++"
...
HOST_TRIPLET = "aarch64-mydevice-linux-gnu"
TARGET_TRIPLET = "$HOST_TRIPLET"
probe = ["$CC --version"]
```

Then a recipe is used for it — and a package that has no `mydevice.toml` or `android.toml`
falls back to its `generic.toml`. Two practical notes:

- **A cross system's tools must not go on `PATH`.** `PATH` and `LD_LIBRARY_PATH` carry
  native dependencies only, so a host tool a cross build needs is a dependency pinned to
  `@native` — which is also what makes it visible in `deps` instead of implied.
- **A cross build may need the NDK's own sysroot rather than ours**, which is what the
  android systems do with `$ANDROID_HOME`. Their `system.toml` script finds the newest NDK
  under `$ANDROID_HOME/ndk`, sets `$SYSROOT` and the compiler names, and fails with a
  message when there is none:

```
: "${ANDROID_HOME:?set ANDROID_HOME to an Android SDK with an NDK}"
```

- **mingw has no `$ORIGIN` to speak of** (a PE file resolves its imports through the
  search path), so the converter emits no runtime path for a mingw system, and a bundle
  gets a launcher instead. That is a fact about the platform, in one place, not a test
  inside every recipe.

## Trying a system

```sh
$ ./builddir/retrolunar --packages ../retrolunar2-packages deps 'zlib@aarch64-android24'
zlib@source 1.3.1
zlib@aarch64-android24
$ ./builddir/retrolunar --packages ../retrolunar2-packages --nest /tmp/an generate -x 'zlib@aarch64-android24'
```

`deps` and `generate` need nothing but the tree, so a package's shape on another system is
one command away. The build itself needs that system's toolchain — the android systems want
a real NDK, and the acceptance does not build them for that reason; the port was checked by
hand on `aarch64-android24` with NDK r29, one package per build system.
