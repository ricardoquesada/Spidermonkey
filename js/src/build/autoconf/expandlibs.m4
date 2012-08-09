AC_DEFUN([MOZ_EXPAND_LIBS],
[
dnl ========================================================
dnl =
dnl = Check what kind of list files are supported by the
dnl = linker
dnl =
dnl ========================================================

AC_CACHE_CHECK(what kind of list files are supported by the linker,
    EXPAND_LIBS_LIST_STYLE,
    [echo "int main() {return 0;}" > conftest.${ac_ext}
     if AC_TRY_COMMAND(${CC-cc} -o conftest.${OBJ_SUFFIX} -c $CFLAGS $CPPFLAGS conftest.${ac_ext} 1>&5) && test -s conftest.${OBJ_SUFFIX}; then
         echo "INPUT(conftest.${OBJ_SUFFIX})" > conftest.list
         if AC_TRY_COMMAND(${CC-cc} -o conftest${ac_exeext} $LDFLAGS conftest.list $LIBS 1>&5) && test -s conftest${ac_exeext}; then
             EXPAND_LIBS_LIST_STYLE=linkerscript
         else
             echo "conftest.${OBJ_SUFFIX}" > conftest.list
             if AC_TRY_COMMAND(${CC-cc} -o conftest${ac_exeext} $LDFLAGS @conftest.list $LIBS 1>&5) && test -s conftest${ac_exeext}; then
                 EXPAND_LIBS_LIST_STYLE=list
             else
                 EXPAND_LIBS_LIST_STYLE=none
             fi
         fi
     else
         dnl We really don't expect to get here, but just in case
         AC_ERROR([couldn't compile a simple C file])
     fi
     rm -rf conftest*])

LIBS_DESC_SUFFIX=desc
AC_SUBST(LIBS_DESC_SUFFIX)
AC_SUBST(EXPAND_LIBS_LIST_STYLE)

if test "$GCC_USE_GNU_LD"; then
    AC_CACHE_CHECK(what kind of ordering can be done with the linker,
        EXPAND_LIBS_ORDER_STYLE,
        [> conftest.order
         _SAVE_LDFLAGS="$LDFLAGS"
         LDFLAGS="${LDFLAGS} -Wl,--section-ordering-file,conftest.order"
         AC_TRY_LINK([], [],
             EXPAND_LIBS_ORDER_STYLE=section-ordering-file,
             EXPAND_LIBS_ORDER_STYLE=)
         LDFLAGS="$_SAVE_LDFLAGS"
         if test -z "$EXPAND_LIBS_ORDER_STYLE"; then
             if AC_TRY_COMMAND(${CC-cc} ${DSO_LDOPTS} ${LDFLAGS} -o ${DLL_PREFIX}conftest${DLL_SUFFIX} -Wl,--verbose 2> /dev/null | sed -n '/^===/,/^===/p' | grep '\.text'); then
                 EXPAND_LIBS_ORDER_STYLE=linkerscript
             else
                 EXPAND_LIBS_ORDER_STYLE=none
             fi
             rm -f ${DLL_PREFIX}conftest${DLL_SUFFIX}
         fi])
fi
AC_SUBST(EXPAND_LIBS_ORDER_STYLE)

])
