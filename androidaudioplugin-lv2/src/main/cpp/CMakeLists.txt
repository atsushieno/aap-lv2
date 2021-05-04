# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.6)

project(androidaudioplugin-lv2 VERSION 0.1 LANGUAGES C CXX)

set(DEPBASE ../../../../dependencies)

if (ANDROID)

if (NOT (BUILD_WITH_PREFAB))
get_filename_component (AAPDIR "${DEPBASE}/android-audio-plugin-framework" ABSOLUTE)
set (androidaudioplugin-lv2_INCLUDES
		"${AAPDIR}/native/plugin-api/include"
		"${AAPDIR}/native/androidaudioplugin/core/include")
link_directories (
		"${AAPDIR}/java/androidaudioplugin/build/intermediates/merged_native_libs/debug/out/lib/${CMAKE_ANDROID_ARCH_ABI}"
)
endif ()

else (ANDROID)

get_filename_component (AAPDIR "${CMAKE_CURRENT_LIST_DIR}/../../../${DEPBASE}/android-audio-plugin-framework" ABSOLUTE)

set (androidaudioplugin-lv2_INCLUDES
		"${AAPDIR}/native/plugin-api/include"
		"${AAPDIR}/native/androidaudioplugin/core/include")

endif (ANDROID)

# resolve dependencies via pkg-config

# set INCLUDE_DIRS and LIBRARY_DIRS

set (androidaudioplugin-lv2_INCLUDES
		${androidaudioplugin-lv2_INCLUDES}
		"src"
		${DEPBASE}/serd/include
		${DEPBASE}/sord
		${DEPBASE}/lv2
		${DEPBASE}/sratom/include
		${DEPBASE}/lilv
		)

# target_link_directories is not usable until cmake 3.14...
link_directories (
)

# set SOURCES. Android build has some additional ones.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set (androidaudioplugin-lv2_INCLUDES
    ${DEPBASE}
	${DEPBASE}/serd/src
	${DEPBASE}/sord/src
	${DEPBASE}/lilv/src
	${androidaudioplugin-lv2_INCLUDES}
	)

set (androidaudioplugin-lv2_SOURCES
		"src/android-audio-plugin-lv2-bridge.cpp"
		"src/AudioPluginLV2LocalHost_native.cpp"
		"src/symap.cpp"
		"src/zix/ring.cpp"

		"src/std_workaround.c"
		"src/abstract_io.c"

		"${DEPBASE}/lilv/src/plugin.c"
		"${DEPBASE}/lilv/src/pluginclass.c"
		"${DEPBASE}/lilv/src/filesystem.c"
		"${DEPBASE}/lilv/src/ui.c"
		"${DEPBASE}/lilv/src/util.c"
		"${DEPBASE}/lilv/src/state.c"
		"${DEPBASE}/lilv/src/lib.c"
		"${DEPBASE}/lilv/src/zix/tree.c"
		# "${DEPBASE}/lilv/src/abstract_io.c"
		"${DEPBASE}/lilv/src/instance.c"
		"${DEPBASE}/lilv/src/node.c"
		"${DEPBASE}/lilv/src/world.c"
		"${DEPBASE}/lilv/src/query.c"
		"${DEPBASE}/lilv/src/scalepoint.c"
		"${DEPBASE}/lilv/src/collections.c"
		"${DEPBASE}/lilv/src/port.c"
		"${DEPBASE}/serd/src/base64.c"
		"${DEPBASE}/serd/src/writer.c"
		"${DEPBASE}/serd/src/system.c"
		"${DEPBASE}/serd/src/reader.c"
		"${DEPBASE}/serd/src/n3.c"
		# "${DEPBASE}/serd/src/abstract_io.c"
		# "${DEPBASE}/serd/src/serdi.c"
		"${DEPBASE}/serd/src/byte_source.c"
		"${DEPBASE}/serd/src/uri.c"
		"${DEPBASE}/serd/src/env.c"
		"${DEPBASE}/serd/src/node.c"
		"${DEPBASE}/serd/src/string.c"
		# "${DEPBASE}/sord/src/sordi.c"
		"${DEPBASE}/sord/src/sord_test.c"
		"${DEPBASE}/sord/src/syntax.c"
		# "${DEPBASE}/sord/src/sord_validate.c"
		"${DEPBASE}/sord/src/zix/hash.c"
		"${DEPBASE}/sord/src/zix/digest.c"
		"${DEPBASE}/sord/src/zix/btree.c"
		"${DEPBASE}/sord/src/sord.c"
		"${DEPBASE}/sratom/src/sratom.c"
		)


add_library (androidaudioplugin-lv2 SHARED ${androidaudioplugin-lv2_SOURCES})

# set LIBS

if (ANDROID)
	if (BUILD_WITH_PREFAB)
		find_package (androidaudioplugin REQUIRED CONFIG)
		set (androidaudioplugin-lv2_LIBS
				android
				log
				androidaudioplugin::androidaudioplugin)
	else (BUILD_WOTH_PREFAB)
		set (androidaudioplugin-lv2_LIBS
				android
				log
				androidaudioplugin)
	endif (BUILD_WITH_PREFAB)
else (ANDROID)
	set (androidaudioplugin-lv2_LIBS
			androidaudioplugin)
endif (ANDROID)

target_include_directories (androidaudioplugin-lv2
        PRIVATE
        ${androidaudioplugin-lv2_INCLUDES}
        )

target_compile_options (androidaudioplugin-lv2
        PRIVATE
        -Wall
        -Wshadow
        # uncomment this if you want to enable AddressSanitizer
        -fsanitize=address -fno-omit-frame-pointer
        )

target_link_libraries (androidaudioplugin-lv2 ${androidaudioplugin-lv2_LIBS})

# uncomment this if you want to enable AddressSanitizer
set_target_properties(androidaudioplugin-lv2 PROPERTIES LINK_FLAGS -fsanitize=address)
