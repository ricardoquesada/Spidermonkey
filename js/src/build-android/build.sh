# options
develop=
release=

usage(){
cat << EOF
usage: $0 [options]

Build SpiderMonkey using Android NDK

OPTIONS:
-d	Build for development
-r  Build for release
-h	this help

EOF
}

while getopts "drh" OPTION; do
case "$OPTION" in
d)
develop=1
;;
r)
release=1
;;
h)
usage
exit 0
;;
esac
done

# configure
../configure --with-android-ndk=$HOME/bin/android-ndk \
             --with-android-sdk=$HOME/bin/android-sdk \
             --with-android-version=14 \
             --with-android-toolchain=$HOME/bin/android-ndk/toolchains/arm-linux-androideabi-4.6/prebuilt/linux-x86 \
             --enable-application=mobile/android \
             --target=arm-linux-androideabi \
             --disable-shared-js \
             --disable-tests \
             --enable-strip \
             --enable-install-strip \
             --enable-debug \
             --disable-methodjit \
             --disable-monoic \
             --disable-polyic

# make
make -j4

if [[ $develop ]]; then
    rm -rf ../../../dist
    ln -s -f "$PWD"/dist -t ../../..
fi

if [[ $release ]]; then
# copy specific files from dist
    rm -rf ../../../dist
    mkdir -p ../../../dist
    mkdir -p ../../../dist/include
    cp -RL dist/include/* ../../../dist/include/
    mkdir -p ../../../dist/lib
    cp -RL dist/lib/libjs_static.a ../../../dist/lib/libjs_static.a

# strip unneeded symbols
    $HOME/bin/android-ndk/toolchains/arm-linux-androideabi-4.6/prebuilt/linux-x86/bin/arm-linux-androideabi-strip --strip-unneeded ../../../dist/lib/libjs_static.a
fi
