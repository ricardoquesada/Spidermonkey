#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug \
             --without-intl-api \
             --disable-threadsafe

# make
../../../build/pymake/make.py -j4
