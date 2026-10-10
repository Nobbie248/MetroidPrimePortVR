#ifndef METROID_PRIME_PORT_PORT_BUILD_INFO_H
#define METROID_PRIME_PORT_PORT_BUILD_INFO_H

// The commit and release version the binary was built from. They're defined in
// a generated source (cmake/BuildRevision.cmake) rather than a generated header,
// so a new commit or a dirty tree recompiles one tiny file instead of every
// file that shows the version.
extern const char kMpBuildRevision[];
extern const char kMpBuildVersion[];

#define MP_BUILD_REVISION kMpBuildRevision
#define MP_BUILD_VERSION kMpBuildVersion

// PrimedGun: the fork's own release version, the set(PRIMEDGUN_LAUNCHER_VERSION ...)
// line in launcher/CMakeLists.txt, which cmake/BuildRevision.cmake writes into the
// same generated source. MP_BUILD_VERSION above stays upstream's version.
extern const char kMpPrimedGunVersion[];
#define MP_PRIMEDGUN_VERSION kMpPrimedGunVersion

#endif
