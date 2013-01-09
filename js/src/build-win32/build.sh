#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug \
             --disable-methodjit \
             --disable-monoic \
             --disable-polyic

# make
make -j4
