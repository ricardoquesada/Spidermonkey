CROSS_ICU_VERSION=50.1.2
TOOLEXEEXT=
TOOLBINDIR=$(cross_buildroot)/bin
TOOLLIBDIR=$(cross_buildroot)/lib
INVOKE=DYLD_LIBRARY_PATH=$(TOOLLIBDIR):$(cross_buildroot)/stubdata:$(cross_buildroot)/tools/ctestfw:$$DYLD_LIBRARY_PATH
PKGDATA_INVOKE=DYLD_LIBRARY_PATH=$(cross_buildroot)/stubdata:$(cross_buildroot)/tools/ctestfw:$(TOOLLIBDIR):$$DYLD_LIBRARY_PATH 

