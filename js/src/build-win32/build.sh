#!/bin/sh

# configure
../configure --disable-tests \
             --enable-strip --enable-strip-install \
             --disable-gcgenerational --disable-exact-rooting \
             --disable-root-analysis --enable-gcincremental \
             --disable-debug \
             --disable-gczeal \
             --without-intl-api \
             --disable-threadsafe

# make
mozmake -j8
