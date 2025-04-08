
ABIS_SIMPLE= x86 x86_64 armeabi-v7a arm64-v8a

all: build-all

build-and-publish: \
	build-aap-core \
	build-lv2-importer \
	create-local-prop \
	publish \

build-all: \
	build-aap-core \
	build-lv2-importer \
	create-local-prop \
	build-java

build-non-app: \
	build-aap-core \
	build-lv2-importer \
	build-java-core

build-aap-core:
	if [ ! -f external/aap-core/local.properties ] ; then \
		if [ `uname` = "Darwin" ] ; then \
			echo "sdk.dir=$(HOME)/Library/Android/sdk" > external/aap-core/local.properties ; \
		else \
			echo "sdk.dir=$(HOME)/Android/Sdk" > external/aap-core/local.properties ; \
		fi ; \
	fi
	cd external/aap-core && ./gradlew publishToMavenLocal

## Build utility

create-local-prop:
	if [ ! -f local.properties ] ; then \
		if [ `uname` = "Darwin" ] ; then \
			echo "sdk.dir=$(HOME)/Library/Android/sdk" > local.properties ; \
		else \
			echo "sdk.dir=$(HOME)/Android/Sdk" > local.properties ; \
		fi ; \
	fi

build-lv2-importer: create-local-prop
	cd tools/aap-import-lv2-metadata && rm -rf build && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make

build-java: create-local-prop
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew build publishToMavenLocal
 
build-java-core: create-local-prop
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew :androidaudioplugin-lv2:build :androidaudioplugin-lv2:publishToMavenLocal

publish: 
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ./gradlew publish
