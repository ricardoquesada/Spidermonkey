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

set -x

host_os=`uname -s | tr "[:upper:]" "[:lower:]"`

# configure
../configure --with-android-ndk=$HOME/bin/android-ndk \
             --with-android-sdk=$HOME/bin/android-sdk \
             --with-android-version=14 \
             --enable-application=mobile/android \
             --with-android-toolchain=$HOME/bin/android-ndk/toolchains/arm-linux-androideabi-4.6/prebuilt/${host_os}-x86 \
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
    rm -rf ../../../include
    rm -rf ../../../lib

    ln -s -f "$PWD"/dist/include ../../..
    ln -s -f "$PWD"/dist/lib ../../..
fi

if [[ $release ]]; then
# copy specific files from dist
    rm -rf ../../../include
    rm -rf ../../../lib
    mkdir -p ../../../include
    cp -RL dist/include/* ../../../include/
    mkdir -p ../../../lib
    cp -RL dist/lib/libjs_static.a ../../../lib/libjs_static.a

# strip unneeded symbols
    $HOME/bin/android-ndk/toolchains/arm-linux-androideabi-4.6/prebuilt/${host_os}-x86/bin/arm-linux-androideabi-strip \
        --strip-unneeded ../../../lib/libjs_static.a
fi
