
# No default rules
.SUFFIXES:

BJIT_BINDIR ?= bin
BJIT_BUILDDIR ?= build

# We assume clang on all platforms
BJIT_USE_CC ?= clang
CC := $(BJIT_USE_CC)

TARGET := bjit

# FIXME: Windows
BINEXT :=

LIBRARY := $(BJIT_BUILDDIR)/$(TARGET).a

MAKEDIR := mkdir -p
CLEANALL := rm -rf $(BJIT_BUILDDIR) $(BJIT_BINDIR)
LINKLIB := libtool -static
LINKBIN := clang

# Generic compilation flags, both C and C++
CFLAGS := -Isrc -g
CFLAGS += -Ofast -fomit-frame-pointer
CFLAGS += -Wall -Werror -Wfloat-conversion -ferror-limit=5
CFLAGS += -Wno-unused -Wno-unused-function

# C++ specific flags
CXXFLAGS := -std=c++11 -fno-exceptions

# Link flags
LINKFLAGS := $(LIBRARY) -lc++

# Automatically figure out source files
LIB_SOURCES := $(wildcard src/*.cpp)

OBJECTS := $(patsubst %,$(BJIT_BUILDDIR)/%.o,$(LIB_SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

# automatic target generation for any subdirectories of test
define TestTarget
 DEPENDS += $(patsubst %,$(BJIT_BUILDDIR)/%.d,$(wildcard $1*.cpp))
 $(BJIT_BINDIR)/$(patsubst test/%/,%,$1)$(BINEXT): $(LIBRARY) \
  $(patsubst %,$(BJIT_BUILDDIR)/%.o,$(wildcard $1*.cpp))
	@echo LINK $$@
	@$(MAKEDIR) "$(BJIT_BINDIR)"
	@$(LINKBIN) -o $$@ $(patsubst %,$(BJIT_BUILDDIR)/%.o,$(wildcard $1*.cpp)) $(LINKFLAGS)
endef

TESTDIRS := $(wildcard test/*/)
TESTS := $(patsubst test/%/,$(BJIT_BINDIR)/%$(BINEXT),$(TESTDIRS))

.PHONY: all clean

all: $(LIBRARY) $(TESTS)
	@echo DONE

clean:
	@$(CLEANALL)
	@echo "Removed '$(BJIT_BUILDDIR)' and '$(BJIT_BINDIR)'"

$(foreach i,$(TESTDIRS),$(eval $(call TestTarget,$(i))))

$(LIBRARY): $(OBJECTS)
	@echo LIB $@
	@$(MAKEDIR) "$(dir $@)"
	@$(LINKLIB) -o $@ $(OBJECTS)

$(BJIT_BUILDDIR)/%.c.o: %.c
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BJIT_BUILDDIR)/%.cpp.o: %.cpp
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPENDS)
