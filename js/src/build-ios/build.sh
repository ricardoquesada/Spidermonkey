#!/bin/sh

## this script is supposed to be run one directory below the original configure script
## usually in build-ios

MIN_IOS_VERSION=5.1
IOS_SDK=8.1

LIPO="xcrun -sdk iphoneos lipo"
STRIP="xcrun -sdk iphoneos strip"

cpus=$(sysctl hw.ncpu | awk '{print $2}')

# remove everything but the static library and this script
#ls | grep -v libjs_static.armv7.a | grep -v libjs_static.armv7s.a | grep -v build.sh | xargs rm -rf

#
# create i386 version (simulator)
#
#../configure --with-ios-target=iPhoneSimulator --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --disable-shared-js --disable-tests --disable-ion --enable-llvm-hacks --enable-debug
../configure --with-ios-target=iPhoneSimulator --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --with-ios-arch=i386 \
            --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --disable-monoic --disable-polyic \
            --enable-optimize=-O3 --enable-strip --enable-install-strip \
            —-disable-debug
make -j$cpus
if (( $? )) ; then
    echo "error when compiling i386 (iOS Simulator) version of the library"
    exit
fi
mv libjs_static.a libjs_static.i386.a


#
# create ios version (armv7)
#
../configure --with-ios-target=iPhoneOS --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --with-ios-arch=armv7 \
            --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --disable-monoic --disable-polyic --disable-yarr-jit \
            --enable-optimize=-O3 --with-thumb=yes --enable-strip --enable-install-strip —-disable-debug
make -j$cpus
if (( $? )) ; then
    echo "error when compiling iOS version of the library"
    exit
fi
mv libjs_static.a libjs_static.armv7.a

#
# done
#
echo "*** You need run ‘build64.sh’ to create arm64 version and fat version ***"
