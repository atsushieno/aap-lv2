# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6.0)

project(ayumi-lv2 VERSION 0.1 LANGUAGES C)

if (ANDROID)

else (ANDROID)

get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../../external/android-audio-plugin-framework" ABSOLUTE)

set (androidaudioplugin-lv2_INCLUDES
        "${AAPDIR}/include")

endif (ANDROID)

set (ayumi-lv2_INCLUDES
	../../../../external/lv2
        )

set (ayumi-lv2_SOURCES
        ../../../../external/ayumi-lv2/ayumi.c
        ../../../../external/ayumi-lv2/ayumi-lv2.c
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

if (BUILD_WITH_PREFAB)
if (ANDROID)
    find_package (androidaudioplugin REQUIRED CONFIG)
    set (ayumi-lv2_LIBS android log androidaudioplugin::androidaudioplugin)
else (ANDROID)
    set (ayumi-lv2_LIBS androidaudioplugin)
endif (ANDROID)
target_link_libraries(ayumi-lv2 ${ayumi-lv2_LIBS})
else (BUILD_WITH_PREFAB)
target_link_libraries(ayumi-lv2 android log)
endif (BUILD_WITH_PREFAB)

# set non-zero this if you want to debug with AddressSanitizer
set(AAP_DEBUG_WITH_ASAN 0)

if (AAP_DEBUG_WITH_ASAN)
    target_compile_options(ayumi-lv2
            PUBLIC
            -fsanitize=address -fno-omit-frame-pointer
            )

    target_link_options(ayumi-lv2
            PUBLIC
            -fsanitize=address
            )

    set_target_properties(ayumi-lv2
            PROPERTIES LINK_FLAGS -fsanitize=address
            )
endif()
