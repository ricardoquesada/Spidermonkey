compile_dirs := config editline shell jsapi-tests gdb
libs_dirs := config editline shell jsapi-tests tests gdb
binaries_dirs := config editline shell jsapi-tests gdb
export_dirs := config shell jsapi-tests tests gdb
tools_dirs := config shell jsapi-tests tests gdb
$(call include_deps,root-deps.mk)
