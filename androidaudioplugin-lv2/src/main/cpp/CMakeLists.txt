# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6)

project(androidaudioplugin-lv2 VERSION 0.1 LANGUAGES CXX)

# Android Studio is stupid. It tries to index your home directory if it references ancestor directories.
# https://issuetracker.google.com/issues/159719671
# Now it references descendant directory via symlink instead.
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

# resolve dependencies via pkg-config

find_package ( PkgConfig REQUIRED )

pkg_search_module ( LV2 REQUIRED lv2>=1.18.0 )
pkg_search_module ( SERD REQUIRED serd-0>=0.28.0 )
pkg_search_module ( SORD REQUIRED sord-0>=0.16.0 )
pkg_search_module ( SRATOM REQUIRED sratom-0>=0.6.0 )
pkg_search_module ( LILV REQUIRED lilv-0>=0.24.0 )

# set INCLUDE_DIRS and LIBRARY_DIRS

set (androidaudioplugin-lv2_INCLUDES
		${androidaudioplugin-lv2_INCLUDES}
		"src"
		${LV2_INCLUDE_DIRS}
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

# set SOURCES. Android build has some additional ones.

set (androidaudioplugin-lv2_SOURCES
		"src/android-audio-plugin-lv2-bridge.cpp"
		"src/AudioPluginLV2LocalHost_native.cpp"
		"src/symap.cpp"
		"src/zix/ring.cpp"
		)


add_library (androidaudioplugin-lv2 SHARED ${androidaudioplugin-lv2_SOURCES})

# set LIBS

if (ANDROID)
	find_package (androidaudioplugin REQUIRED CONFIG)
	set (androidaudioplugin-lv2_LIBS
			android
			log
			${LILV_LIBRARIES}
			androidaudioplugin::androidaudioplugin)
else (ANDROID)
	set (androidaudioplugin-lv2_LIBS
			${LILV_LIBRARIES}
			androidaudioplugin)
endif (ANDROID)

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
