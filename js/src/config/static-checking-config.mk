# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

DEHYDRA_SCRIPT = $(topsrcdir)/config/static-checking.js

DEHYDRA_MODULES = \
  $(NULL)

TREEHYDRA_MODULES = \
  $(topsrcdir)/jsstack.js \
  $(NULL)

DEHYDRA_ARG_PREFIX=-fplugin-arg-gcc_treehydra-

DEHYDRA_ARGS = \
  $(DEHYDRA_ARG_PREFIX)script=$(DEHYDRA_SCRIPT) \
  $(DEHYDRA_ARG_PREFIX)topsrcdir=$(topsrcdir) \
  $(DEHYDRA_ARG_PREFIX)objdir=$(DEPTH) \
  $(DEHYDRA_ARG_PREFIX)dehydra-modules=$(subst $(NULL) ,$(COMMA),$(strip $(DEHYDRA_MODULES))) \
  $(DEHYDRA_ARG_PREFIX)treehydra-modules=$(subst $(NULL) ,$(COMMA),$(strip $(TREEHYDRA_MODULES))) \
  $(NULL)

DEHYDRA_FLAGS = -fplugin=$(DEHYDRA_PATH) $(DEHYDRA_ARGS)

ifdef DEHYDRA_PATH
OS_CXXFLAGS += $(DEHYDRA_FLAGS)
endif

ifdef ENABLE_CLANG_PLUGIN
# Load the clang plugin from the mozilla topsrcdir. This implies that the clang
# plugin is only usable if we're building js/src under mozilla/, though.
CLANG_PLUGIN := $(DEPTH)/../../build/clang-plugin/$(DLL_PREFIX)clang-plugin$(DLL_SUFFIX)
OS_CXXFLAGS += -fplugin=$(CLANG_PLUGIN)
OS_CFLAGS += -fplugin=$(CLANG_PLUGIN)
endif
