# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6.0)

project(ayumi-lv2 VERSION 0.1 LANGUAGES C)

if (ANDROID)

get_filename_component (LV2_DEP_DIST "${CMAKE_CURRENT_LIST_DIR}/symlinked-dist/${CMAKE_ANDROID_ARCH_ABI}" ABSOLUTE)
set (ENV{PKG_CONFIG_PATH} "${LV2_DEP_DIST}/lib/pkgconfig/")

else (ANDROID)

get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../../dependencies/android-audio-plugin-framework" ABSOLUTE)
get_filename_component (LV2_DEP_DIST "${AAPDIR}/dependencies/lv2-desktop/dist" ABSOLUTE)

set (androidaudioplugin-lv2_INCLUDES
        "${AAPDIR}/native/plugin-api/include"
        "${AAPDIR}/native/androidaudioplugin/core/include")

endif (ANDROID)

find_package ( PkgConfig REQUIRED )

pkg_search_module ( LV2 REQUIRED lv2>=1.18.0 )

set (ayumi-lv2_INCLUDES
        ${LV2_INCLUDE_DIRS}
        )

set (ayumi-lv2_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/ayumi-lv2/ayumi.c
        ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/ayumi-lv2/ayumi-lv2.c
        )

add_library ( # Specifies the name of the library.
        ayumi-lv2

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        ${ayumi-lv2_SOURCES}
        )

target_include_directories (ayumi-lv2
        PRIVATE
        ${ayumi-lv2_INCLUDES}
        )

target_compile_options(ayumi-lv2
        PRIVATE
        -Wall
        -Wextra-semi
        -Wshadow
        -Wshadow-field
        )

if (ANDROID)
    find_package (androidaudioplugin REQUIRED CONFIG)
    set (ayumi-lv2_LIBS android log androidaudioplugin::androidaudioplugin)
else (ANDROID)
    set (ayumi-lv2_LIBS androidaudioplugin)
endif (ANDROID)

target_link_libraries(ayumi-lv2 android log)

