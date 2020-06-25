
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-all: \
	get-lv2-deps \
	import-lv2-deps \
	get-sfizz-deps \
	patch-sfizz \
	get-guitarix-deps \
	import-guitarix-deps \
	build-java

patch-sfizz: dependencies/sfizz/patch.stamp

dependencies/sfizz/patch.stamp:
	cd dependencies/sfizz && \
		patch -i ../../sfizz-android.patch -p1 && \
		touch patch.stamp || exit 1

## downloads

get-lv2-deps: dependencies/dist/stamp

dependencies/dist/stamp: android-lv2-binaries.zip
	mkdir -p dependencies
	unzip android-lv2-binaries -d dependencies
	./rewrite-pkg-config-paths.sh
	touch dependencies/dist/stamp

android-lv2-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r4/android-lv2-binaries.zip

get-sfizz-deps: dependencies/sfizz-deps/stamp

dependencies/sfizz-deps/stamp: android-libsndfile-binaries.zip
	unzip android-libsndfile-binaries.zip -d dependencies/sfizz-deps/
	for a in $(ABIS_SIMPLE) ; do \
		mkdir -p aap-sfizz/src/main/jniLibs/$$a ; \
		cp -R dependencies/sfizz-deps/dist/$$a/lib/*.so aap-sfizz/src/main/jniLibs/$$a ; \
	done
	touch dependencies/sfizz-deps/stamp

android-libsndfile-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r6/android-libsndfile-binaries.zip

get-guitarix-deps: dependencies/guitarix-deps/stamp

dependencies/guitarix-deps/stamp: aap-guitarix-binaries.zip
	unzip aap-guitarix-binaries.zip -d dependencies/guitarix-deps/
	for a in $(ABIS_SIMPLE) ; do \
		mkdir -p aap-guitarix/src/main/jniLibs/$$a ; \
		cp -R dependencies/guitarix-deps/dist/$$a/lib/*.so aap-guitarix/src/main/jniLibs/$$a ; \
	done
	touch dependencies/guitarix-deps/stamp

aap-guitarix-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/r6/aap-guitarix-binaries.zip

# Run importers

import-lv2-deps: build-lv2-importer
	mkdir -p java/samples/aaphostsample/src/main/res/xml
	./import-lv2-deps.sh

import-guitarix-deps: build-lv2-importer
	./import-guitarix-deps.sh

## Build utility

build-lv2-importer:
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew assembleDebug
 
