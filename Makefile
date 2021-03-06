
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-all: \
	build-aap-core \
	build-lv2-importer \
	build-java

build-non-app: \
	build-aap-core \
	build-lv2-importer \
	build-java-core

build-aap-core:
	cd dependencies/android-audio-plugin-framework && make all-no-desktop

## Build utility

build-lv2-importer:
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew build publishToMavenLocal
 
build-java-core:
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew :androidaudioplugin-lv2:build :androidaudioplugin-lv2:publishToMavenLocal

 
