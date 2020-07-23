# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6.0)


get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../../android-audio-plugin-framework" ABSOLUTE)

if (ANDROID)
    get_filename_component (LV2_DEP_DIST "${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/dist/${CMAKE_ANDROID_ARCH_ABI}" ABSOLUTE)
else (ANDROID)
    get_filename_component (LV2_DEP_DIST "${AAPDIR}/dependencies/lv2-desktop/dist" ABSOLUTE)
endif (ANDROID)

set (ayumi-lv2_INCLUDES
        ${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/dist/${CMAKE_ANDROID_ARCH_ABI}/include
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

target_link_libraries(ayumi-lv2 android log)

