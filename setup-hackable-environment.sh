#!/bin/bash

CURDIR=$(dirname $0)
NATIVE_BUILDER=$(realpath "${CURDIR}/../android-native-audio-builders")

REPOS="serd sord lv2 sratom lilv"
HACKTOP=androidaudioplugin-lv2/src/main/cpp/src/lilv_direct

mkdir -p $HACKTOP
for NAME in $REPOS ; do
	echo "link and copy configs in: " ${NAME}
	rm -f ${HACKTOP}/${NAME}
	ln -s ${NATIVE_BUILDER}/${NAME} ${HACKTOP}/${NAME}
	if [ ${NAME} != 'lv2' ] ; then
		cp $NATIVE_BUILDER/build/x86/${NAME}/build/${NAME}_config.h ${HACKTOP} ;
	fi ;
done

cp ${NATIVE_BUILDER}/serd/src/abstract_io.* ${HACKTOP}

rm -rf ${HACKTOP}/../../../jniLibs

wget https://gist.githubusercontent.com/atsushieno/969eedaeefb51d99309a3234c2f9b8de/raw/a8e4863ae3d90993a2c47112c2bfa8ce6d9d643e/aap-lv2-direct.patch

patch -i aap-lv2-direct.patch -p1
