#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug \
             --without-intl-api

# make
make -j4
