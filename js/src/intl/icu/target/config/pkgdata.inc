GENCCODE_ASSEMBLY_TYPE=-a gcc-darwin
SO=dylib
SOBJ=dylib
A=a
LIBPREFIX=lib
LIB_EXT_ORDER=.50.1.2.dylib
COMPILE=/usr/bin/clang -DU_ATTRIBUTE_DEPRECATED= -DU_USING_ICU_NAMESPACE=0 -DU_NO_DEFAULT_INCLUDE_UTF_HEADERS=1 -DUCONFIG_NO_LEGACY_CONVERSION -DUCONFIG_NO_TRANSLITERATION -DUCONFIG_NO_REGULAR_EXPRESSIONS -DUCONFIG_NO_BREAK_ITERATION -Qunused-arguments     -fPIC -Qunused-arguments -Wall -Wpointer-arith -Wdeclaration-after-statement -Werror=return-type -Wempty-body -Wsign-compare -Wno-unused -std=gnu99 -fno-common -fno-math-errno -pthread -pipe -g -UDEBUG -DNDEBUG -O3 -fno-stack-protector -Wall -std=c99 -pedantic -Wshadow -Wpointer-arith -Wmissing-prototypes -Wwrite-strings  -fno-common -c
LIBFLAGS=-I/usr/local/include  -dynamic
GENLIB=/usr/bin/clang -dynamiclib -dynamic -fPIC -Qunused-arguments -Wall -Wpointer-arith -Wdeclaration-after-statement -Werror=return-type -Wempty-body -Wsign-compare -Wno-unused -std=gnu99 -fno-common -fno-math-errno -pthread -pipe -g -UDEBUG -DNDEBUG -O3 -fno-stack-protector -Wall -std=c99 -pedantic -Wshadow -Wpointer-arith -Wmissing-prototypes -Wwrite-strings  -framework ExceptionHandling   -lobjc  
LDICUDTFLAGS=
LD_SONAME=-Wl,-compatibility_version -Wl,50 -Wl,-current_version -Wl,50.1.2 -install_name @executable_path/
RPATH_FLAGS=
BIR_LDFLAGS=
AR=ar
ARFLAGS=r -c
RANLIB=ranlib
INSTALL_CMD=/usr/bin/install -c
