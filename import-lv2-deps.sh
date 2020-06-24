#!/bin/bash

ABIS_SIMPLE=(x86 x86_64 armeabi-v7a arm64-v8a)

# Remove existing jniLibs for sanity
rm -rf androidaudioplugin-lv2/src/main/jniLibs/
rm -rf aap-mda-lv2/src/main/jniLibs/


for abi in ${ABIS_SIMPLE[*]}
do
    echo "ABI: $abi"
    # Copy native libs for each ABI
    mkdir -p androidaudioplugin-lv2/src/main/jniLibs/$abi
    cp -R dependencies/dist/$abi/lib/*.so androidaudioplugin-lv2/src/main/jniLibs/$abi/
    # And then copy native libs of LV2 plugins for each ABI.
    mkdir -p aap-mda-lv2/src/main/jniLibs/$abi
    cp -R dependencies/dist/$abi/lib/lv2/*/*.so aap-mda-lv2/src/main/jniLibs/$abi/
done

# Copy LV2 metadata files etc.
# The non-native parts should be the same so we just copy files from x86 build.
#
# Also, we only need `mda.lv2` here. Avoid copying those LV2 (SDK) files.
# But not that when you are copying this script, you should probably
# NOT name a specific plugin.
#
mkdir -p aap-mda-lv2/src/main/assets/lv2
cp -R dependencies/dist/x86/lib/lv2/mda.lv2 aap-mda-lv2/src/main/assets/lv2/
# ... except for *.so files. They are stored under jniLibs.
rm aap-mda-lv2/src/main/assets/lv2/*/*.so

# Generate `aap-metadata.xml` that AAP service look up plugins.
mkdir -p aap-mda-lv2/src/main/assets/lv2
mkdir -p aap-mda-lv2/src/main/res/xml
tools/aap-import-lv2-metadata/build/aap-import-lv2-metadata \
	aap-mda-lv2/src/main/assets/lv2 \
	aap-mda-lv2/src/main/res/xml

