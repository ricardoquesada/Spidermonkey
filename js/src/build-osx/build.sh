#!/bin/sh

# configure
../configure --disable-tests \
             --enable-debug

# make
make -j4
