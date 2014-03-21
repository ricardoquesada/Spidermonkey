jsapi-tests/compile:
editline/compile:
shell/compile:
config/compile:
gdb/compile:
recurse_compile: config/compile editline/compile shell/compile jsapi-tests/compile gdb/compile
tests/libs: jsapi-tests/libs
shell/libs: editline/libs
jsapi-tests/libs: shell/libs
gdb/libs: tests/libs
editline/libs: config/libs
config/libs:
recurse_libs: gdb/libs
jsapi-tests/binaries:
editline/binaries:
shell/binaries:
config/binaries:
gdb/binaries:
recurse_binaries: config/binaries editline/binaries shell/binaries jsapi-tests/binaries gdb/binaries
jsapi-tests/export:
tests/export:
shell/export:
config/export:
gdb/export:
recurse_export: config/export shell/export jsapi-tests/export tests/export gdb/export
jsapi-tests/tools: shell/tools
tests/tools: jsapi-tests/tools
shell/tools: config/tools
config/tools:
gdb/tools: tests/tools
recurse_tools: gdb/tools
