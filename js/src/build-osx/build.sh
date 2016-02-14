#!/bin/sh

cpus=$(sysctl hw.ncpu | awk '{print $2}')

# configure
../configure --disable-tests --disable-shared-js \
            --enable-strip --enable-strip-install \
            --disable-gcgenerational --disable-exact-rooting \
            --disable-root-analysis --enable-gcincremental --enable-optimize=-O3 \
            --enable-llvm-hacks \
            --disable-debug \
            --disable-gczeal \
            --without-intl-api \
            --disable-threadsafe
# make
xcrun make -j$cpus

# strip
xcrun strip -S js/src/libjs_static.a

# info
xcrun lipo -info js/src/libjs_static.a

mv js/src/libjs_static.a libjs_static.a

