#!/bin/sh

# configure
../configure --disable-tests \
             --disable-debug 

# make
make -j4
