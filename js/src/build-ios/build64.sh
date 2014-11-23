#!/bin/sh

## this script is supposed to be run one directory below the original configure script
## usually in build-ios

MIN_IOS_VERSION=5.1
IOS_SDK=8.1

LIPO="xcrun -sdk iphoneos lipo"
STRIP="xcrun -sdk iphoneos strip"

cpus=$(sysctl hw.ncpu | awk '{print $2}')


# back up 32-bit libraries
mkdir tmp
mv libjs_static* tmp/
make distclean

# remove everything but the static library and this script
#ls | grep -v libjs_static.armv7.a | grep -v libjs_static.armv7s.a | grep -v build.sh | xargs rm -rf

#
# create ios version (arm64)
#
../configure --with-ios-target=iPhoneOS --with-ios-version=$IOS_SDK --with-ios-min-version=$MIN_IOS_VERSION --with-ios-arch=arm64 \
            --disable-shared-js --disable-tests --disable-ion --disable-jm --disable-tm --enable-llvm-hacks --disable-methodjit --disable-monoic --disable-polyic --disable-yarr-jit \
            --enable-optimize=-O3 --with-thumb=yes --enable-strip --enable-install-strip --enable-intl-api=no —-disbable-debug
make -j$cpus
if (( $? )) ; then
    echo "error when compiling iOS(arm64) version of the library"
    exit
fi
mv libjs_static.a libjs_static.arm64.a

# restore 32-bit libraries
mv tmp/* .
rmdir tmp

#
# lipo create
#
if [ -e libjs_static.arm64.a ] ; then
    echo "creating fat version of the library"
    $LIPO -create -output libjs_static.a libjs_static.i386.a libjs_static.armv7.a libjs_static.arm64.a
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
