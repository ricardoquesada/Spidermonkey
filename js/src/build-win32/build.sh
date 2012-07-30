#!/bin/sh

# configure
../configure --disable-tests \
             --enable-debug \
             --disable-methodjit \
             --disable-monoic \
             --disable-polyic

# make
make -j4
