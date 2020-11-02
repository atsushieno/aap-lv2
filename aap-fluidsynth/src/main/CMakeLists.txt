# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6.0)


get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/android-audio-plugin-framework" ABSOLUTE)

if (ANDROID)
    get_filename_component (LV2_DEP_DIST "${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/lv2-deps/dist/${CMAKE_ANDROID_ARCH_ABI}" ABSOLUTE)
else (ANDROID)
    get_filename_component (LV2_DEP_DIST "${AAPDIR}/dependencies/lv2-desktop/dist" ABSOLUTE)
endif (ANDROID)

set (aap-fluidsynth_INCLUDES
        ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/lv2-deps/dist/${CMAKE_ANDROID_ARCH_ABI}/include
        include
        ${AAPDIR}/native/androidaudioplugin/android/include
        )

set (aap-fluidsynth_SOURCES
        fluidsynth-lv2.cpp
        fluid_androidasset.c
        )

add_library ( # Specifies the name of the library.
        aap-fluidsynth

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        ${aap-fluidsynth_SOURCES}
        )

if (ANDROID)
    get_filename_component(AAP_ANDROID_PLAT_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/android-audio-plugin-framework/native/androidaudioplugin/android/include ABSOLUTE)
    get_filename_component(AAP_REF_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/android-audio-plugin-framework/java/androidaudioplugin/build/intermediates/library_jni/debug/jni/${CMAKE_ANDROID_ARCH_ABI} ABSOLUTE)
    target_include_directories (aap-fluidsynth PUBLIC "${AAP_ANDROID_PLAT_INCLUDE_DIR}")
    target_link_directories (aap-fluidsynth
            PUBLIC
            "../../../dependencies/fluidsynth-deps/dist/${CMAKE_ANDROID_ARCH_ABI}"
            "${AAP_REF_LIB_DIR}")
endif (ANDROID)

target_include_directories (aap-fluidsynth
        PRIVATE
        ${aap-fluidsynth_INCLUDES}
        )

target_link_directories (aap-fluidsynth
        PRIVATE
        jniLibs/${CMAKE_ANDROID_ARCH_ABI})

target_compile_options (aap-fluidsynth
        PRIVATE
        -Wall
        -Wextra-semi
        -Wshadow
        -Wshadow-field
        )

target_link_libraries (aap-fluidsynth android log androidaudioplugin fluidsynth fluidsynth-assetloader)

