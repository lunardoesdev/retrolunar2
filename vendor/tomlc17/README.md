# tomlc17

Vendored, not a subproject dependency: the parser ships as two files and building it
in-tree keeps `meson setup` free of downloads.

Upstream: https://github.com/cktan/tomlc17 — MIT, see `LICENSE`.

- revision `fba134d`
- `TOMLC17_RELEASE_AFTER` in `tomlc17.h` says `260618`
- only `tomlc17.c` and `tomlc17.h` are taken; the C++ wrapper and the test suite are not.

tomlc17 validates no schema: it hands back a generic tree. Our own typed reader on top of
it (`src/toml_read.c`) is what turns an unknown key, a bad type or a missing section into a
message naming the file and the line.
