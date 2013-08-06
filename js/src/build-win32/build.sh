#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug \
             --enable-intl-api=no

# make
make -j4
