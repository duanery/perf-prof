# SPDX-License-Identifier: GPL-2.0
# # Some of the tools (perf) use same make variables
# # as in kernel build.
export srctree=$(CURDIR)
export objtree=$(CURDIR)

ifeq ("$(origin V)", "command line")
  VERBOSE = $(V)
endif
ifndef VERBOSE
  VERBOSE = 0
endif

ifeq ($(VERBOSE),1)
  Q =
else
  Q = @
endif

MAKEFLAGS += --no-print-directory

all: __build
include $(srctree)/scripts/Makefile.include
include $(srctree)/build/Makefile.include
include $(srctree)/scripts/Makefile.arch

INCLUDES = \
-I$(srctree)/lib/perf/include \
-I$(srctree)/lib/traceevent \
-I$(srctree)/lib/subcmd \
-I$(srctree)/lib/ \
-I$(srctree)/include \
-I$(srctree)/include/uapi \
-I$(srctree)/arch/$(SRCARCH)/include \
-I$(srctree)/arch/$(SRCARCH)/include/uapi \
-I$(srctree)/ \
-I$(srctree)/src/


EXTRA_CFLAGS := -O3 -g

# Append required CFLAGS
override CFLAGS += $(EXTRA_WARNINGS) $(EXTRA_CFLAGS)
override CFLAGS += -Werror -Wall -Wno-shadow
override CFLAGS += $(INCLUDES)
override CFLAGS += -fvisibility=hidden
override CXXFLAGS += $(EXTRA_CFLAGS) $(INCLUDES)

export srctree OUTPUT CFLAGS CXXFLAGS V EXTRA_CFLAGS SRCARCH INCLUDES EXTRA_WARNINGS

ifeq ($(filter clean,$(MAKECMDGOALS)),)
include $(srctree)/Makefile.config
endif

bin = $(Q)$(MAKE) -f $(srctree)/build/Makefile.bin dir=. $@

# The sub-make is hidden inside $(bin), so make cannot spot the literal
# $(MAKE) in the recipe and will not pass the jobserver down ("warning:
# jobserver unavailable: using -j1"). The '+' prefix marks the line as a
# recursive make invocation explicitly.
#
# Build fixdep serially first: every lib/*/Makefile declares 'all: fixdep',
# so under -j they would otherwise race each other writing the same
# build/fixdep.o and build/fixdep binary.
__build: fixdep
	+$(bin)

clean: fixdep-clean
	+$(bin)
	$(Q)rm -f $(OUTPUT)FEATURE-DUMP $(OUTPUT).config-detected
