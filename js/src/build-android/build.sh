# options
develop=
release=
RELEASE_DIR="spidermonkey-android"
ARCH=armv6
ARCH_DIR=armeabi

usage(){
cat << EOF
usage: $0 [options]

Build SpiderMonkey using Android NDK

OPTIONS:
-d	Build for development
-r  Build for release. specify RELEASE_DIR.
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

build_with_arch()
{
../configure --with-android-ndk=$HOME/bin/android-ndk \
             --with-android-sdk=$HOME/bin/android-sdk \
             --with-android-version=9 \
             --enable-application=mobile/android \
             --with-android-gnu-compiler-version=4.6 \
             --with-arch=$ARCH \
             --enable-android-libstdcxx \
             --target=arm-linux-androideabi \
             --disable-shared-js \
             --disable-tests \
             --enable-strip \
             --enable-install-strip \
             --disable-debug \
             --disable-ion \
             --disable-jm \
             --disable-tm

# make
make -j15

if [[ $develop ]]; then
    rm -rf ../../../include
    rm -rf ../../../lib

    ln -s -f "$PWD"/dist/include ../../..
    ln -s -f "$PWD"/dist/lib ../../..
fi

if [[ $release ]]; then
# copy specific files from dist
    rm -r "$RELEASE_DIR/include"
    rm -r "$RELEASE_DIR/lib/$ARCH_DIR"
    mkdir -p "$RELEASE_DIR/include"
    cp -RL dist/include/* "$RELEASE_DIR/include/"
    mkdir -p "$RELEASE_DIR/lib/$ARCH_DIR"
    cp -L dist/lib/libjs_static.a "$RELEASE_DIR/lib/$ARCH_DIR/libjs_static.a"

# strip unneeded symbols
    $HOME/bin/android-ndk/toolchains/arm-linux-androideabi-4.6/prebuilt/${host_os}-x86/bin/arm-linux-androideabi-strip \
        --strip-unneeded "$RELEASE_DIR/lib/$ARCH_DIR/libjs_static.a"
fi

}

# Build with armv6
build_with_arch

# Build with armv7
ARCH=armv7-a
ARCH_DIR=armeabi-v7a

build_with_arch
