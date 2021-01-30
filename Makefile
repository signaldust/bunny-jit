
# No default rules
.SUFFIXES:

BIN_DIR ?= bin
BUILD_DIR ?= build

# We assume clang on all platforms
CC := clang

TARGET := bjit

# FIXME: Windows
BINEXT :=

LIBRARY := $(BUILD_DIR)/$(TARGET).a

MAKEDIR := mkdir -p
CLEANALL := rm -rf $(BUILD_DIR) $(BIN_DIR)
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

OBJECTS := $(patsubst %,$(BUILD_DIR)/%.o,$(LIB_SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

# automatic target generation for any subdirectories of test
define TestTarget
 DEPENDS += $(patsubst %,$(BUILD_DIR)/%.d,$(wildcard $1*.cpp))
 $(BIN_DIR)/$(patsubst test/%/,%,$1)$(BINEXT): $(LIBRARY) \
  $(patsubst %,$(BUILD_DIR)/%.o,$(wildcard $1*.cpp))
	@echo LINK $$@
	@$(MAKEDIR) "$(BIN_DIR)"
	@$(LINKBIN) -o $$@ $(patsubst %,$(BUILD_DIR)/%.o,$(wildcard $1*.cpp)) $(LINKFLAGS)
endef

TESTDIRS := $(wildcard test/*/)
TESTS := $(patsubst test/%/,$(BIN_DIR)/%$(BINEXT),$(TESTDIRS))

.PHONY: all clean

all: $(LIBRARY) $(TESTS)
	@echo DONE

clean:
	@echo Cleaning '$(BUILD_DIR)' and '$(BIN_DIR)'
	@$(CLEANALL)

$(foreach i,$(TESTDIRS),$(eval $(call TestTarget,$(i))))

$(LIBRARY): $(OBJECTS)
	@echo LIB $@
	@$(MAKEDIR) "$(dir $@)"
	@$(LINKLIB) -o $@ $(OBJECTS)

$(BUILD_DIR)/%.c.o: %.c
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.cpp.o: %.cpp
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPENDS)
