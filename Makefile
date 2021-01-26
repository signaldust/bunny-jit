
# No default rules
.SUFFIXES:

BUILD_DIR ?= build

# We assume clang on all platforms
CC := clang

MAKEDIR := mkdir -p

# Generic compilation flags, both C and C++
CFLAGS := -I.
CFLAGS += -Ofast -fomit-frame-pointer
CFLAGS += -Wall -Werror -Wfloat-conversion -ferror-limit=5
CFLAGS += -Wno-unused -Wno-unused-function

# C++ specific flags
CXXFLAGS := -std=c++11 -fno-exceptions

# Link flags
LDFLAGS := -lc++

# Automatically figure out source files
SOURCES := $(wildcard src/*.cpp)

OBJECTS := $(patsubst %,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

TARGET := bjit

.PHONY: all clean

all: $(TARGET)
	@echo DONE

clean:
	@echo Cleaning directory \'$(BUILD_DIR)\'
	@$(CLEANALL)

$(TARGET): $(OBJECTS)
	@echo LINK $@
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

$(BUILD_DIR)/%.c.o: %.c
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.cpp.o: %.cpp
	@echo CC $<
	@$(MAKEDIR) "$(dir $@)"
	@$(CC) -MMD -MP $(CFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPENDS)
