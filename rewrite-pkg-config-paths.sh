#!/bin/bash

PWD=`pwd`
PWDESC=${PWD//\//\\\/}
DESTDIR=$1

for f in `find dependencies/$DESTDIR/dist/*/lib/pkgconfig -name *.pc` ; do
	sed -e "s/\/home\/runner\/work\/android-native-audio-builders\/android-native-audio-builders\/cerbero-artifacts\/cerbero\/build/$PWDESC\/dependencies\/$DESTDIR/g" $f \
		| sed -e "s/android_x86/x86/g" \
		| sed -e "s/android_armv7/armeabi-v7a/g" \
		| sed -e "s/android_arm64/arm64-v8a/g" \
		> $f.1;
	sed -e "s/\/home\/runner\/work\/android-native-audio-builders\/android-native-audio-builders/$PWDESC\/dependencies\/$DESTDIR/g" $f.1 > $f.2 ;
	mv $f.2 $f
	rm $f.1
done
