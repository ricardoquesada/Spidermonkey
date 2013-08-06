#!/bin/sh

## this script is supposed to be run one directory below the original configure script
## usually in build-ios

MIN_IOS_VERSION=4.3
IOS_SDK=6.1

LIPO="xcrun -sdk iphoneos lipo"
STRIP="xcrun -sdk iphoneos strip"

cpus=$(sysctl hw.ncpu | awk '{print $2}')

# remove everything but the static library and this script
#ls | grep -v libjs_static.armv7.a | grep -v libjs_static.armv7s.a | grep -v build.sh | xargs rm -rf

#
# create i386 version (simulator)
#
#../configure --with-ios-target=iPhoneSimulator --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --disable-shared-js --disable-tests --disable-ion --enable-llvm-hacks --enable-debug
../configure --with-ios-target=iPhoneSimulator --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION \
            --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --disable-monoic --disable-polyic \
            --enable-optimize=-O3 --enable-strip --enable-install-strip \
            --enable-debug --enable-intl-api=no
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
            --enable-optimize=-O3 --with-thumb=yes --enable-strip --enable-install-strip --enable-intl-api=no
make -j$cpus
if (( $? )) ; then
    echo "error when compiling iOS version of the library"
    exit
fi
mv libjs_static.a libjs_static.armv7.a


#
# create ios version (armv7s)
#

#../configure --with-ios-target=iPhoneOS --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --with-ios-arch=armv7s  --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --with-thumb=yes --enable-strip --enable-install-strip --disable-monoic --disable-polyic --disable-ion --enable-optimize=-O1
../configure --with-ios-target=iPhoneOS --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --with-ios-arch=armv7s \
            --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --disable-monoic --disable-polyic --disable-yarr-jit \
            --enable-optimize=-O3 --with-thumb=yes --enable-strip --enable-install-strip --enable-intl-api=no
make -j$cpus
if (( $? )) ; then
    echo "error when compiling iOS version of the library"
    exit
fi
mv libjs_static.a libjs_static.armv7s.a


#
# lipo create
#
if [ -e libjs_static.i386.a ]  && [ -e libjs_static.armv7.a ] && [ -e libjs_static.armv7s.a ] ; then
    echo "creating fat version of the library"
    $LIPO -create -output libjs_static.a libjs_static.i386.a libjs_static.armv7.a libjs_static.armv7s.a
    # remove debugging info
    $STRIP -S libjs_static.a
    $LIPO -info libjs_static.a
fi

#
# done
#
echo "*** DONE ***"
echo "If you want to use spidermonkey, copy the 'dist' directory to some accesible place"
echo "e.g. 'cp -pr dist ~/path/to/your/project'"
echo "and then add the proper search paths for headers and libraries in your Xcode project"
