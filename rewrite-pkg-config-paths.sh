#!/bin/bash

PWD=`pwd`
PWDESC=${PWD//\//\\\/}

for f in `find dependencies/dist/*/lib/pkgconfig -name *.pc` ; do
	sed -e "s/\/home\/runner\/work\/android-native-audio-builders\/android-native-audio-builders\/cerbero-artifacts\/cerbero\/build/$PWDESC\/dependencies/g" $f > $f.1;
	sed -e "s/\/home\/runner\/work\/android-native-audio-builders\/android-native-audio-builders/$PWDESC\/dependencies/g" $f.1 > $f.2 ;
	mv $f.2 $f
	rm $f.1
done
