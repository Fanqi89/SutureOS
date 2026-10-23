/* SutureOS: single source of truth for the release version.
 *
 * The string MUST be a valid SemVer 2.0.0 version so that the git tag
 * (v<version>), the boot banner, the shell welcome line and the ISO manifest
 * all agree. scripts/build_iso.py parses this header, so bumping the version is
 * a one-line change.
 *
 * Release discipline: every batch of changes gets a new `vX.Y.Z[-pre]` tag with
 * an annotated message describing what that version does; existing tags are
 * never moved or deleted, so old and new versions coexist in the repository.
 */
#ifndef SUTUREOS_VERSION_H
#define SUTUREOS_VERSION_H

#define SUTUREOS_VERSION_MAJOR 0
#define SUTUREOS_VERSION_MINOR 13
#define SUTUREOS_VERSION_PATCH 0
#define SUTUREOS_VERSION_PRE   "alpha"

#define SUTUREOS_VERSION_STRINGIFY_(x) #x
#define SUTUREOS_VERSION_STRINGIFY(x)  SUTUREOS_VERSION_STRINGIFY_(x)

#define SUTUREOS_VERSION \
    SUTUREOS_VERSION_STRINGIFY(SUTUREOS_VERSION_MAJOR) "." \
    SUTUREOS_VERSION_STRINGIFY(SUTUREOS_VERSION_MINOR) "." \
    SUTUREOS_VERSION_STRINGIFY(SUTUREOS_VERSION_PATCH) "-" SUTUREOS_VERSION_PRE

#endif /* SUTUREOS_VERSION_H */
