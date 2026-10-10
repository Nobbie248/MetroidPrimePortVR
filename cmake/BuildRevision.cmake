set(revision "source-snapshot")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-toplevel
        OUTPUT_VARIABLE git_root OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(git_root STREQUAL SOURCE_DIR)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
            OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" diff --quiet HEAD --
            RESULT_VARIABLE dirty ERROR_QUIET)
        if(NOT dirty EQUAL 0)
            string(APPEND revision "-dirty")
        endif()
    endif()
endif()
# The release version is versionName in the Android build file, which every
# release bumps (docs/RELEASING.md); empty when it can't be read.
set(version "")
if(EXISTS "${SOURCE_DIR}/android/app/build.gradle")
    file(STRINGS "${SOURCE_DIR}/android/app/build.gradle" version_line REGEX "versionName \"[0-9.]+\"")
    if(version_line MATCHES "versionName \"([0-9.]+)\"")
        set(version "${CMAKE_MATCH_1}")
    endif()
endif()
# PrimedGun: the fork's own release version is the set(PRIMEDGUN_LAUNCHER_VERSION ...)
# line in launcher/CMakeLists.txt, which every PrimedGun release bumps together with
# versionName in quest/app/build.gradle.kts. MP_BUILD_VERSION above stays upstream's;
# this one is kMpPrimedGunVersion (MP_PRIMEDGUN_VERSION in platform/include's
# port_build_info.h), empty when it can't be read.
set(primedgun_version "")
if(EXISTS "${SOURCE_DIR}/launcher/CMakeLists.txt")
    file(STRINGS "${SOURCE_DIR}/launcher/CMakeLists.txt" primedgun_line
        REGEX "set\\(PRIMEDGUN_LAUNCHER_VERSION \"[^\"]+\"\\)")
    if(primedgun_line MATCHES "set\\(PRIMEDGUN_LAUNCHER_VERSION \"([^\"]+)\"\\)")
        set(primedgun_version "${CMAKE_MATCH_1}")
    endif()
endif()
# extern: a namespace-scope const is otherwise internal, and the include can find a
# stale generated port_build_info.h (the old scheme's, next to this file) instead
# of the declaring header.
set(content "#include \"port_build_info.h\"\nextern const char kMpBuildRevision[] = \"${revision}\";\nextern const char kMpBuildVersion[] = \"${version}\";\nextern const char kMpPrimedGunVersion[] = \"${primedgun_version}\";\n")
set(previous "")
if(EXISTS "${OUTPUT_SOURCE}")
    file(READ "${OUTPUT_SOURCE}" previous)
endif()
if(NOT previous STREQUAL content)
    get_filename_component(output_dir "${OUTPUT_SOURCE}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    file(WRITE "${OUTPUT_SOURCE}" "${content}")
endif()
