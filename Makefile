# ============================================================================
# SQLite Memorized Views Extension - Makefile
# ============================================================================

PROJECT = mview
SRC = mview_extension.c
BUILD_DIR = build
DB_DIR = databases
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

# Create build and database directories
directory:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(DB_DIR)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(DB_DIR)
	rm -f *.db *.db-wal *.db-shm
	rm -rf __pycache__ $(TEST_DIR)/__pycache__
	rm -f *.gcov *.gcda *.gcno

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
	@# Also pass DB_DIR via TEST_DB_PATH
	@EXT_PATH=$(TARGET_LIB) TEST_DB_PATH=$(DB_DIR)/test_cache.db $(PYTHON) $(TEST_DIR)/test_mview.py
	@echo "Tests Passed!"


# ----------------------------------------------------------------------------
# Code Quality Goals (100% Coverage & No Leaks)
# ----------------------------------------------------------------------------

coverage: clean directory
	@# 1. Compile with coverage flags
	$(CC) $(CFLAGS) $(INCLUDES) -fprofile-arcs -ftest-coverage -c $(SRC) -o $(BUILD_DIR)/mview_extension.o
	$(CC) $(LDFLAGS) -fprofile-arcs -ftest-coverage -o $(TARGET_LIB) $(BUILD_DIR)/mview_extension.o

	@# 2. Run Tests to generate .gcda data
	@echo "Running Tests for Coverage..."
	@EXT_PATH=$(TARGET_LIB) TEST_DB_PATH=$(DB_DIR)/test_cache.db $(PYTHON) $(TEST_DIR)/test_mview.py

	@# 3. Generate Report
	@echo "Generating Coverage Report..."
	gcov $(SRC) -o $(BUILD_DIR)
	@mv -f *.gcov $(BUILD_DIR)/ 2>/dev/null || true
	@echo "Coverage Report Generated: $(BUILD_DIR)/mview_extension.c.gcov"

leak-check: clean directory
	@# 1. Compile Extension (Standard Debug)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $(TARGET_LIB) $(SRC)

	@# 2. Compile Leak Checker
	$(CC) -g $(TEST_DIR)/leak_check.c -lsqlite3 -o $(BUILD_DIR)/leak_check

	@# 3. Run Valgrind (ignore 'still reachable' blocks from SQLite internals)
	@echo "Running Valgrind Memory Check..."
	@cd $(BUILD_DIR) && TEST_DB_PATH=../$(DB_DIR)/leak_test_cache.db valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --error-exitcode=1 ./leak_check

check: test leak-check coverage

.PHONY: all clean directory windows windows32 macos_universal test coverage leak-check check
