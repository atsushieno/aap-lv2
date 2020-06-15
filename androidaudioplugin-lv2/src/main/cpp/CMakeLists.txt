# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.5.1)

# List of sources. Android build has some additional sources.
set (androidaudioplugin-lv2_SOURCES
	"src/android-audio-plugin-lv2-bridge.cpp"
	)

set (AAPDIR "../../../../../android-audio-plugin-framework")

if (ANDROID)

set (LV2_DEP_DIST "${AAPDIR}/dependencies/dist/${CMAKE_ANDROID_ARCH_ABI}")

# it is not usable until cmake 3.14...
#target_link_directories (androidaudioplugin-lv2 ...)
link_directories (
		"${LV2_DEP_DIST}/lib/"
		"${AAPDIR}/java/androidaudioplugin/build/intermediates/merged_native_libs/debug/out/lib/${CMAKE_ANDROID_ARCH_ABI}"
)

else (ANDROID)

set (LV2_DEP_DIST "${AAPDIR}/dependencies/lv2-desktop/dist")

# it is not usable until cmake 3.14...
target_link_directories (androidaudioplugin-lv2
		"${LV2_DEP_DIST}/lib/"
)

endif (ANDROID)

# set includes and libs

set (ENV{PKG_CONFIG_PATH} "${LV2_DEP_DIST}/lib/pkgconfig/")

if (ANDROID)

set (androidaudioplugin-lv2_INCLUDES
	"${AAPDIR}/native/plugin-api/include"
	"${AAPDIR}/native/androidaudioplugin/core/include"
	"${LV2_DEP_DIST}/include/lilv-0"
	"${LV2_DEP_DIST}/include"
	)

set (androidaudioplugin-lv2_LIBS ${androidaudioplugin_LIBS} android log)

else (ANDROID)

set (androidaudioplugin-lv2_INCLUDES
	"${AAPDIR}/native/plugin-api/include"
	"${AAPDIR}/native/androidaudioplugin/core/include"
	"${LV2_DEP_DIST}/include/lilv-0"
	"${LV2_DEP_DIST}/include"
	)

set (androidaudioplugin-lv2_LIBS ${androidaudioplugin_LIBS})

endif (ANDROID)


find_package ( PkgConfig REQUIRED )

# Mandatory libraries: serd, sord, lilv
pkg_check_modules ( SERD REQUIRED serd-0>=0.28.0 )
pkg_check_modules ( SORD REQUIRED sord-0>=0.16.0 )
pkg_check_modules ( SRATOM REQUIRED sratom-0>=0.6.0 )
pkg_check_modules ( LILV REQUIRED lilv-0>=0.24.0 )

set (androidaudioplugin-lv2_SOURCES
	${androidaudioplugin-lv2_SOURCES}
	"src/android-audio-plugin-lv2-bridge.cpp"
	"src/AudioPluginLV2LocalHost_native.cpp"
	)

set (androidaudioplugin-lv2_INCLUDES
	${androidaudioplugin-lv2_INCLUDES}
	${SERD_INCLUDE_DIRS}
	${SORD_INCLUDE_DIRS}
	${SRATOM_INCLUDE_DIRS}
	${LILV_INCLUDE_DIRS}
	)

set (androidaudioplugin-lv2_LIBS ${androidaudioplugin-lv2_LIBS} lilv-0 androidaudioplugin)

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
