#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug \
             --disable-ion \
             --disable-jm \
             --disable-tm

# make
make -j4
