
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-all: \
        build-aap-core \
	get-lv2-deps \
	import-lv2-deps \
	get-fluidsynth-deps \
	get-sfizz-deps \
	patch-sfizz \
	get-guitarix-deps \
	import-guitarix-deps \
	build-java

build-aap-core:
	cd dependencies/android-audio-plugin-framework && make all-no-desktop

patch-sfizz: dependencies/sfizz/patch.stamp

dependencies/sfizz/patch.stamp:
	cd dependencies/sfizz && \
		patch -i ../../sfizz-android.patch -p1 && \
		touch patch.stamp || exit 1

## downloads

get-lv2-deps: dependencies/lv2-deps/dist/stamp

dependencies/lv2-deps/dist/stamp: android-lv2-binaries.zip
	mkdir -p dependencies/lv2-deps
	unzip android-lv2-binaries -d dependencies/lv2-deps/
	./rewrite-pkg-config-paths.sh lv2-deps
	ln -s `pwd`/dependencies/lv2-deps/dist androidaudioplugin-lv2/src/main/cpp/symlinked-dist
	touch dependencies/lv2-deps/dist/stamp

android-lv2-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r4/android-lv2-binaries.zip

get-fluidsynth-deps: dependencies/fluidsynth-deps/dist/stamp

dependencies/fluidsynth-deps/dist/stamp: android-fluidsynth-dist.zip
	unzip android-fluidsynth-dist.zip -d dependencies/fluidsynth-deps
	ln -s `pwd`/dependencies/fluidsynth-deps/dist aap-fluidsynth/src/main/jniLibs
	touch dependencies/fluidsynth-deps/dist/stamp

android-fluidsynth-dist.zip:
	wget https://581-95304991-gh.circle-artifacts.com/0/android-dist.zip -O android-fluidsynth-dist.zip

get-sfizz-deps: dependencies/sfizz-deps/dist/stamp

dependencies/sfizz-deps/dist/stamp: android-libsndfile-binaries.zip
	unzip android-libsndfile-binaries.zip -d dependencies/sfizz-deps/
	./rewrite-pkg-config-paths.sh sfizz-deps
	for a in $(ABIS_SIMPLE) ; do \
		mkdir -p aap-sfizz/src/main/jniLibs/$$a ; \
		cp -R dependencies/sfizz-deps/dist/$$a/lib/*.so aap-sfizz/src/main/jniLibs/$$a ; \
	done
	touch dependencies/sfizz-deps/dist/stamp

android-libsndfile-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r6/android-libsndfile-binaries.zip

get-guitarix-deps: dependencies/guitarix-deps/dist/stamp

dependencies/guitarix-deps/dist/stamp: aap-guitarix-binaries.zip
	unzip aap-guitarix-binaries.zip -d dependencies/guitarix-deps/
	for a in $(ABIS_SIMPLE) ; do \
		mkdir -p aap-guitarix/src/main/jniLibs/$$a ; \
		cp -R dependencies/guitarix-deps/dist/$$a/lib/*.so aap-guitarix/src/main/jniLibs/$$a ; \
	done
	touch dependencies/guitarix-deps/dist/stamp

aap-guitarix-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r6/aap-guitarix-binaries.zip

# Run importers

import-lv2-deps: build-lv2-importer
	./import-lv2-deps.sh

import-guitarix-deps: build-lv2-importer
	./import-guitarix-deps.sh

## Build utility

build-lv2-importer:
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew build
 
