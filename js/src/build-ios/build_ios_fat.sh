#!/bin/sh

MIN_IOS_VERSION=4.0
IOS_VERSION=5.1

#../configure --target=arm-apple-darwin10 --with-ios-target=iPhoneOS --with-ios-version=$IOS_VERSION --with-ios-min-version=$MIN_IOS_VERSION --with-thumb=yes --disable-shared-js --disable-tests --enable-strip --enable-install-strip
#../configure --target=arm-apple-darwin10 --with-ios-target=iPhoneOS --with-ios-version=$IOS_VERSION --with-ios-min-version=$MIN_IOS_VERSION --with-thumb=yes --disable-shared-js --disable-tests --enable-strip --enable-install-strip --disable-optimize
../configure --disable-methodjit --target=arm-apple-darwin10 --with-ios-target=iPhoneOS --with-ios-version=$IOS_VERSION --with-ios-min-version=$MIN_IOS_VERSION --with-thumb=yes --disable-shared-js --disable-tests --enable-strip --enable-install-strip --enable-optimize=-O1
make -j4
mv libjs_static.a libjs_static_armv7.a

../configure --disable-methodjit --target=arm-apple-darwin10 --with-ios-target=iPhoneSimulator --with-ios-version=$IOS_VERSION --with-ios-min-version=$MIN_IOS_VERSION --disable-shared-js --disable-tests --enable-strip --enable-install-strip --enable-debug
make -j4
mv libjs_static.a libjs_static_i386.a

# make fat library
lipo -create -output libjs_static.a libjs_static_i386.a libjs_static_armv7.a
# strip the library
strip -S libjs_static.a

# create dist build
if [ -n "$DISTRIBUTION" ]; then
	cp -RL dist spidermonkey-ios
	zip -r spidermonkey-ios.zip spidermonkey-ios
	rm -rf spidermonkey-ios
fi

