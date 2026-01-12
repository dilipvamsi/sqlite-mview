# ============================================================================
# SQLite Memorized Views Extension - Makefile
# ============================================================================

PROJECT = mview
SRC = mview_extension.c
BUILD_DIR = build
TEST_DIR = tests

# Compiler Settings
CC ?= gcc
CFLAGS = -O2 -g -fPIC -Wall
INCLUDES = -I.

# Python for testing (Can be overridden: make test PYTHON=python)
PYTHON ?= python3

# Auto-detect Operating System
UNAME_S := $(shell uname -s)

# ----------------------------------------------------------------------------
# Platform / Target Detection
# ----------------------------------------------------------------------------

# If TARGET_LIB is not passed as an argument (e.g., via CI), detect defaults.
ifndef TARGET_LIB
	# Linux
	ifeq ($(UNAME_S),Linux)
		TARGET_LIB = $(BUILD_DIR)/$(PROJECT).so
		LDFLAGS ?= -shared
	endif

	# macOS (Darwin)
	ifeq ($(UNAME_S),Darwin)
		TARGET_LIB = $(BUILD_DIR)/$(PROJECT).dylib
		# SQLite symbols are in the host application
		LDFLAGS ?= -dynamiclib -undefined dynamic_lookup
	endif

	# Windows (MinGW/MSYS detection)
	ifneq (,$(findstring MINGW,$(UNAME_S)))
		TARGET_LIB = $(BUILD_DIR)/$(PROJECT).dll
		LDFLAGS ?= -shared
	endif
	ifneq (,$(findstring MSYS,$(UNAME_S)))
		TARGET_LIB = $(BUILD_DIR)/$(PROJECT).dll
		LDFLAGS ?= -shared
	endif
endif

# Default LDFLAGS if detection failed or wasn't set
LDFLAGS ?= -shared

# ----------------------------------------------------------------------------
# Targets
# ----------------------------------------------------------------------------

all: clean directory $(TARGET_LIB)
	@echo "Build successful! Output: $(TARGET_LIB)"

# Main Build Rule
# Uses whatever CC, CFLAGS, LDFLAGS, and TARGET_LIB are currently set
$(TARGET_LIB): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $<

# Create build directory
directory:
	@mkdir -p $(BUILD_DIR)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.db *.db-wal *.db-shm
	rm -rf __pycache__ $(TEST_DIR)/__pycache__

# ----------------------------------------------------------------------------
# Specific Build Targets (Convenience / Cross-Compilation)
# ----------------------------------------------------------------------------

# Build for Windows 64-bit (Run on Linux)
windows: directory
	x86_64-w64-mingw32-gcc -shared -o $(BUILD_DIR)/$(PROJECT).dll $(SRC) -I.

# Build for Windows 32-bit (Run on Linux)
windows32: directory
	i686-w64-mingw32-gcc -shared -o $(BUILD_DIR)/$(PROJECT)_32.dll $(SRC) -I.

# Build for macOS Universal (x86_64 + arm64) (Run on macOS)
macos_universal: directory
	$(CC) $(CFLAGS) $(INCLUDES) -dynamiclib -undefined dynamic_lookup -arch x86_64 -arch arm64 -o $(BUILD_DIR)/$(PROJECT).dylib $(SRC)

# ----------------------------------------------------------------------------
# Testing
# ----------------------------------------------------------------------------

test: all
	@echo "Running Tests..."
	@# Pass the path of the compiled library to Python via Environment Variable
	@EXT_PATH=$(TARGET_LIB) $(PYTHON) $(TEST_DIR)/test_mview.py
	@echo "Tests Passed!"

.PHONY: all clean directory windows windows32 macos_universal test
