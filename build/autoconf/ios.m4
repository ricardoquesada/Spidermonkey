dnl This Source Code Form is subject to the terms of the Mozilla Public
dnl License, v. 2.0. If a copy of the MPL was not distributed with this
dnl file, You can obtain one at http://mozilla.org/MPL/2.0/.

dnl ========================================================
dnl = First test to make it work for iOS
dnl = Xcode >= 4.3.1 required
dnl ========================================================

AC_DEFUN([MOZ_IOS_SDK],
[

MOZ_ARG_WITH_STRING(ios-version,
[  --with-ios-version=VER
                      version of the iOS SDK, defaults to 6.0],
    ios_sdk_version=$withval,
    ios_sdk_version=8.1)

MOZ_ARG_WITH_STRING(ios-min-version,
[  --with-ios-min-version=VER
                          deploy target version, defaults to 4.3],
    ios_deploy_version=$withval,
    ios_deploy_version=5.1.1)

MOZ_ARG_WITH_STRING(ios-arch,
[  --with-ios-arch=ARCH
                   iOS architecture, defaults to armv7 for device, x86 for simulator],
    ios_arch=$withval)


case "$ios_target" in
iPhoneOS|iPhoneSimulator)
    dnl test for Xcode 4.3+
    if ! test -d `xcode-select --print-path` ; then
        AC_MSG_ERROR([You must install Xcode first from the App Store])
    fi

    if test "$ios_target" == "iPhoneSimulator" ; then
        if test "$ios_arch" == "i386"; then
            dnl force ios_arch to i386 for simulator
            CPU_ARCH=i386
            ios_arch=i386
        fi

        if test "$ios_arch" == "x86_64"; then
            dnl force ios_arch to x86_64 for simulator
            CPU_ARCH=x86_64
            ios_arch=x86_64
        fi

        target_name=x86
        target=i386-darwin
        TARGET_CPU=i386
    else
        if test "$ios_arch" == "armv7"; then
            CPU_ARCH=armv7
            ios_arch=armv7
            TARGET_CPU=armv7
        fi

        if test "$ios_arch" == "armv7s"; then
            CPU_ARCH=armv7s
            ios_arch=armv7s
            TARGET_CPU=armv7s
        fi

        if test "$ios_arch" == "armv64"; then
            CPU_ARCH=arm64
            ios_arch=arm64
            TARGET_CPU=arm64
        fi

        target_name=arm
        target=arm-darwin
        DISABLE_YARR_JIT=1
        AC_SUBST(DISABLE_YARR_JIT)
    fi
    target_os=darwin

    xcode_base="`xcode-select --print-path`/Platforms"
    ios_sdk_root=""
    ios_toolchain="`xcode-select --print-path`/Toolchains/XcodeDefault.xctoolchain/usr/bin"

    dnl test to see if the actual sdk exists
    ios_sdk_root="$xcode_base"/$ios_target.platform/Developer/SDKs/$ios_target"$ios_sdk_version".sdk
    if ! test -d "$ios_sdk_root" ; then
        AC_MSG_ERROR([Invalid SDK version])
    fi

    dnl set the compilers
    AS="$ios_toolchain"/as
    CC="$ios_toolchain"/clang
    CXX="$ios_toolchain"/clang++
    CPP="$ios_toolchain/clang -E"
    LD="$ios_toolchain"/ld
    AR="$ios_toolchain"/ar
    RANLIB="$ios_toolchain"/ranlib
    STRIP="$ios_toolchain"/strip
    LDFLAGS="-isysroot $ios_sdk_root -arch $ios_arch -v"

    if test "$ios_target" == "iPhoneSimulator" ; then
        CFLAGS="-isysroot $ios_sdk_root -arch $ios_arch -mios-simulator-version-min=$ios_deploy_version -I$ios_sdk_root/usr/include -pipe -Wno-implicit-int -Wno-return-type"
    else
        CFLAGS="-isysroot $ios_sdk_root -arch $ios_arch -miphoneos-version-min=$ios_deploy_version -I$ios_sdk_root/usr/include -pipe -Wno-implicit-int -Wno-return-type"
    fi
    
    CXXFLAGS="$CFLAGS"
    CPPFLAGS="$CFLAGS"

    dnl prevent cross compile section from using these flags as host flags
    if test -z "$HOST_CPPFLAGS" ; then
        HOST_CPPFLAGS=" "
    fi
    if test -z "$HOST_CFLAGS" ; then
        HOST_CFLAGS=" "
    fi
    if test -z "$HOST_CXXFLAGS" ; then
        HOST_CXXFLAGS=" "
    fi
    if test -z "$HOST_LDFLAGS" ; then
        HOST_LDFLAGS=" "
    fi

    AC_DEFINE(IPHONEOS)
    CROSS_COMPILE=1
esac
])