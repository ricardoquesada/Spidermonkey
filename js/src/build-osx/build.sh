#!/bin/sh

# configure
../configure --disable-tests \
             --enable-debug

# make
make -j4

# strip
strip -S libjs_static.a
