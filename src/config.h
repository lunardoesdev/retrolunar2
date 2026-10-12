/* Compile-time configuration. The default system is the native one: a bare
 * target resolves to it, and '@native' is an alias for the same identity. */
#ifndef RETROLUNAR_CONFIG_H
#define RETROLUNAR_CONFIG_H

#ifndef RETROLUNAR_DEFAULT_SYSTEM
#define RETROLUNAR_DEFAULT_SYSTEM "clang-native"
#endif

#define RETROLUNAR_PACKAGES_REPO "https://github.com/lunardoesdev/retrolunar-packages"

#define RETROLUNAR_VERSION "2.0.0"

#endif /* RETROLUNAR_CONFIG_H */
