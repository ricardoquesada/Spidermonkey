#!/bin/sh
set -e

cpus=`nproc`

echo "$cpus cpus detected"

# configure
../configure --disable-tests \
            --disable-shared-js \
            --enable-strip \
            --enable-strip-install \
            --disable-root-analysis \
            --disable-exact-rooting \
            --enable-optimize=-O3 \
            --enable-llvm-hacks \
            --disable-debug \
            --without-intl-api \
            --disable-threadsafe
# make
make clean
make -j$cpus

# strip
strip -S libjs_static.a

