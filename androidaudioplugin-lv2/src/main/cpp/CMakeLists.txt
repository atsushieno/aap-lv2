# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.5.1)

get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../../dependencies/android-audio-plugin-framework" ABSOLUTE)

# Android Studio is stupid. It tries to index your home directory if it references ancestor directories.
# https://issuetracker.google.com/issues/159719671
# Now it references descendant directory via symlink instead.
if (ANDROID)
get_filename_component (LV2_DEP_DIST "${CMAKE_CURRENT_LIST_DIR}/symlinked-dist/${CMAKE_ANDROID_ARCH_ABI}" ABSOLUTE)
else (ANDROID)
get_filename_component (LV2_DEP_DIST "${AAPDIR}/dependencies/lv2-desktop/dist" ABSOLUTE)
endif (ANDROID)

# set resolve dependencies via pkg-config

set (ENV{PKG_CONFIG_PATH} "${LV2_DEP_DIST}/lib/pkgconfig/")

find_package ( PkgConfig REQUIRED )

# Mandatory libraries: serd, sord, lilv
pkg_search_module ( SERD REQUIRED serd-0>=0.28.0 )
pkg_search_module ( SORD REQUIRED sord-0>=0.16.0 )
pkg_search_module ( SRATOM REQUIRED sratom-0>=0.6.0 )
pkg_search_module ( LILV REQUIRED lilv-0>=0.24.0 )

# set INCLUDE_DIRS and LIBRARY_DIRS

set (androidaudioplugin-lv2_INCLUDES
		"${AAPDIR}/native/plugin-api/include"
		"${AAPDIR}/native/androidaudioplugin/core/include"
		"src"
		${SERD_INCLUDE_DIRS}
		${SORD_INCLUDE_DIRS}
		${SRATOM_INCLUDE_DIRS}
		${LILV_INCLUDE_DIRS}
		)

# target_link_directories is not usable until cmake 3.14...
link_directories (
		${SERD_LIBRARY_DIRS}
		${SORD_LIBRARY_DIRS}
		${SRATOM_LIBRARY_DIRS}
		${LILV_LIBRARY_DIRS}
)
if (ANDROID)
link_directories (
		"${AAPDIR}/java/androidaudioplugin/build/intermediates/merged_native_libs/debug/out/lib/${CMAKE_ANDROID_ARCH_ABI}"
)
endif (ANDROID)

# set LIBS

if (ANDROID)
set (androidaudioplugin-lv2_LIBS ${androidaudioplugin_LIBS} android log)
else (ANDROID)
set (androidaudioplugin-lv2_LIBS ${androidaudioplugin_LIBS})
endif (ANDROID)
set (androidaudioplugin-lv2_LIBS ${androidaudioplugin-lv2_LIBS} lilv-0 androidaudioplugin)

# set SOURCES. Android build has some additional ones.

set (androidaudioplugin-lv2_SOURCES
		"src/android-audio-plugin-lv2-bridge.cpp"
		"src/AudioPluginLV2LocalHost_native.cpp"
		"src/symap.cpp"
		"src/zix/ring.cpp"
		)

# Specifies a library name, specifies whether the library is STATIC or
# SHARED, and provides relative paths to the source code. You can
# define multiple libraries by adding multiple add_library() commands,
# and CMake builds them for you. When you build your app, Gradle
# automatically packages shared libraries with your APK.

add_library ( # Specifies the name of the library.
            androidaudioplugin-lv2

            # Sets the library as a shared library.
            SHARED

            # Provides a relative path to your source file(s).
            ${androidaudioplugin-lv2_SOURCES}
            )

# Either prefab support in Android Studio is quite buggy or the documentation is wrong, and it never worked.
# It simply fails to generate *-config.cmake constructs, most likely `prefab` tool installation is missing (AGP issue).
#find_package(aap_lv2_natives REQUIRED CONFIG)

target_include_directories (androidaudioplugin-lv2
        PRIVATE
        ${androidaudioplugin-lv2_INCLUDES}
        )

target_compile_options (androidaudioplugin-lv2
        PRIVATE
        -std=c++17
        -Wall
        -Wshadow
        )

target_link_libraries (androidaudioplugin-lv2 ${androidaudioplugin-lv2_LIBS})
