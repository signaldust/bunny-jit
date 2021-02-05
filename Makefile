
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
CFLAGS := -Isrc -g -ferror-limit=5
CFLAGS += -Ofast -fomit-frame-pointer
CFLAGS += -Wall -Werror -Wfloat-conversion -Wno-unused-function

# C++ specific flags
CXXFLAGS := -std=c++11 -fno-exceptions

# Link flags
LINKFLAGS := $(LIBRARY) -lc++

# Automatically figure out source files
LIB_SOURCES := $(wildcard src/*.cpp)

LIB_OBJECTS := $(patsubst %,$(BJIT_BUILDDIR)/%.o,$(LIB_SOURCES))
DEPENDS := $(LIB_OBJECTS:.o=.d)

# Front-end
FRONTEND := $(BJIT_BINDIR)/$(TARGET)$(BINEXT)
FRONT_OBJECTS := $(patsubst %,$(BJIT_BUILDDIR)/%.o,$(wildcard front/*.cpp))
DEPENDS += $(FRONT_OBJECTS:.o=.d)

# automatic target generation for any .cpp files in tests/
define TestTarget
 DEPENDS += $(patsubst %,$(BJIT_BUILDDIR)/%.d,$1)
 $(BJIT_BINDIR)/$(patsubst tests/%.cpp,%,$1)$(BINEXT): $(LIBRARY) \
  $(patsubst %,$(BJIT_BUILDDIR)/%.o,$1)
	@echo LINK $$@
	@$(MAKEDIR) "$(BJIT_BINDIR)"
	@$(LINKBIN) -o $$@ $(patsubst %,$(BJIT_BUILDDIR)/%.o,$1) $(LINKFLAGS)
endef

TESTS_CPP := $(wildcard tests/*.cpp)
TESTS := $(patsubst tests/%.cpp,$(BJIT_BINDIR)/%$(BINEXT),$(TESTS_CPP))

.PHONY: all test clean

all: $(LIBRARY) $(FRONTEND) $(TESTS)
	@echo DONE

test: all
	@echo Running tests with output to 'test.out'
	@/bin/bash -e ./run-tests.sh > test.out 2>&1 || /bin/bash -e ./run-tests.sh
	@echo Tests done.
    
clean:
	@$(CLEANALL)
	@echo "Removed '$(BJIT_BUILDDIR)' and '$(BJIT_BINDIR)'"

$(foreach i,$(TESTS_CPP),$(eval $(call TestTarget,$(i))))

$(FRONTEND): $(FRONT_OBJECTS) $(LIBRARY)
	@echo LINK $@
	@$(MAKEDIR) "$(BJIT_BINDIR)"
	@$(LINKBIN) -o $@ $(FRONT_OBJECTS) $(LINKFLAGS)

$(LIBRARY): $(LIB_OBJECTS)
	@echo LIB $@
	@$(MAKEDIR) "$(dir $@)"
	@$(LINKLIB) -o $@ $(LIB_OBJECTS)

$(BJIT_BUILDDIR)/%.c.o: %.c
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BJIT_BUILDDIR)/%.cpp.o: %.cpp
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPENDS)
