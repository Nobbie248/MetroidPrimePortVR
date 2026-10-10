# libprimedgun_launcher.so: the Quest launcher's native half (quest/app,
# org.primedgun.v2.launcher.LauncherNative), the launcher core behind JNI.
# Built with -DMP_BUILD_QUEST_LAUNCHER=ON by quest/app/build.gradle.kts.
#
# The build revision (MP_BUILD_REVISION in platform/include/port_build_info.h) is
# defined in the root's generated source since upstream 074e531a, so it is built
# into this library too. The generated folder is not on the include path: in an
# older build folder it still holds a stale port_build_info.h.
add_library(primedgun_launcher SHARED primedgun_launcher_jni.cpp
    "${PROJECT_BINARY_DIR}/generated/port_build_info.cpp")
target_include_directories(primedgun_launcher PRIVATE "${PROJECT_SOURCE_DIR}/platform/include")
target_link_libraries(primedgun_launcher PRIVATE primedgun_launcher_core)
add_dependencies(primedgun_launcher port_build_info)
set_target_properties(primedgun_launcher_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Only the Java_* entry points are exported; the core and the C++ runtime stay
# private to this library (each .so of the APK has its own c++_static copy).
set_target_properties(primedgun_launcher PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_options(primedgun_launcher PRIVATE "-Wl,--exclude-libs,ALL")
