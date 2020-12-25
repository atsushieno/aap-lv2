
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-all: \
	build-aap-core \
	get-lv2-deps \
	import-lv2-deps \
	build-java

build-non-app: \
	build-aap-core \
	get-lv2-deps \
	import-lv2-deps \
	build-lv2-importer \
	build-java-core

build-aap-core:
	cd dependencies/android-audio-plugin-framework && make all-no-desktop

## downloads

get-lv2-deps: dependencies/lv2-deps/dist/stamp

dependencies/lv2-deps/dist/stamp: android-lv2-binaries.zip
	mkdir -p dependencies/lv2-deps
	unzip android-lv2-binaries -d dependencies/lv2-deps/
	./rewrite-pkg-config-paths.sh lv2-deps
	if [ ! -d androidaudioplugin-lv2/src/main/cpp/symlinked-dist ] ; then \
		ln -s `pwd`/dependencies/lv2-deps/dist androidaudioplugin-lv2/src/main/cpp/symlinked-dist ; \
	fi
	if [ ! -d androidaudioplugin-lv2/src/main/cpp/symlinked-dist ] ; then \
		ln -s `pwd`/dependencies/lv2-deps/dist aap-ayumi/src/main/symlinked-dist ; \
	fi
	touch dependencies/lv2-deps/dist/stamp

android-lv2-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r8.1/android-lv2-binaries.zip

# Run importers

import-lv2-deps: build-lv2-importer
	./import-lv2-deps.sh

## Build utility

build-lv2-importer:
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew build publishToMavenLocal
 
build-java-core:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew :androidaudioplugin-lv2:build :androidaudioplugin-lv2:publishToMavenLocal

 
