
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-all: \
	get-lv2-deps \
	import-lv2-deps \
	build-java

get-lv2-deps: dependencies/dist/stamp

dependencies/dist/stamp: android-lv2-binaries.zip
	mkdir -p dependencies
	unzip android-lv2-binaries -d dependencies
	./rewrite-pkg-config-paths.sh
	touch dependencies/dist/stamp

android-lv2-binaries.zip:
	wget https://github.com/atsushieno/android-native-audio-builders/releases/download/refs/heads/r1/android-lv2-binaries.zip

import-lv2-deps: build-lv2-importer
	mkdir -p java/samples/aaphostsample/src/main/res/xml
	./import-lv2-deps.sh

build-lv2-importer:
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew assemble
 
