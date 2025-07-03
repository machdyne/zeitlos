# Makefile for ZObj Test Suite
# Assumes zobj.c and zobj.h are in a different directory

# Configuration
ZOBJ_DIR = ../common
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -O0
LDFLAGS = 
INCLUDES = -I$(ZOBJ_DIR)

# Directories
BUILD_DIR = build
TEST_DIR = tests

# Source files
ZOBJ_SRC = $(ZOBJ_DIR)/zobj.c
TEST_SRC = test_zobj.c
ADDITIONAL_TEST_SRC = $(wildcard $(TEST_DIR)/test_*.c)

# Object files
ZOBJ_OBJ = $(BUILD_DIR)/zobj.o
TEST_OBJ = $(BUILD_DIR)/test_zobj.o
ADDITIONAL_TEST_OBJ = $(ADDITIONAL_TEST_SRC:$(TEST_DIR)/%.c=$(BUILD_DIR)/%.o)

# Executables
TEST_EXE = $(BUILD_DIR)/test_zobj
ADDITIONAL_TEST_EXE = $(ADDITIONAL_TEST_SRC:$(TEST_DIR)/test_%.c=$(BUILD_DIR)/test_%)

# Default target
all: $(TEST_EXE)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build zobj object file
$(ZOBJ_OBJ): $(ZOBJ_SRC) $(ZOBJ_DIR)/zobj.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build main test object file
$(TEST_OBJ): $(TEST_SRC) $(ZOBJ_DIR)/zobj.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build additional test object files
$(BUILD_DIR)/test_%.o: $(TEST_DIR)/test_%.c $(ZOBJ_DIR)/zobj.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build main test executable
$(TEST_EXE): $(TEST_OBJ) $(ZOBJ_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Build additional test executables
$(BUILD_DIR)/test_%: $(BUILD_DIR)/test_%.o $(ZOBJ_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Test targets
test: $(TEST_EXE)
	@echo "Running main test suite..."
	./$(TEST_EXE)

test-all: $(TEST_EXE) $(ADDITIONAL_TEST_EXE)
	@echo "Running main test suite..."
	./$(TEST_EXE)
	@echo ""
	@for test in $(ADDITIONAL_TEST_EXE); do \
		echo "Running $$test..."; \
		./$$test; \
		echo ""; \
	done

# Memory testing with valgrind
memtest: $(TEST_EXE)
	@echo "Running memory test with valgrind..."
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TEST_EXE)

# Quick test (just compile and run)
quick: $(TEST_EXE)
	./$(TEST_EXE)

# Debug build
debug: CFLAGS += -DDEBUG -g3
debug: $(TEST_EXE)

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: $(TEST_EXE)

# Coverage build (requires gcov)
coverage: CFLAGS += --coverage
coverage: LDFLAGS += --coverage
coverage: $(TEST_EXE)
	./$(TEST_EXE)
	gcov $(ZOBJ_SRC) $(TEST_SRC)
	@echo "Coverage files generated. Use 'make coverage-report' to view."

# Generate coverage report
coverage-report:
	@echo "=== Coverage Report ==="
	@gcov -r $(ZOBJ_SRC) | grep -E "(File|Lines executed)"

# Static analysis with cppcheck
analyze:
	@echo "Running static analysis..."
	cppcheck --enable=all --std=c99 $(INCLUDES) $(ZOBJ_SRC) $(TEST_SRC)

# Format code with clang-format
format:
	@echo "Formatting code..."
	clang-format -i $(TEST_SRC) $(wildcard $(TEST_DIR)/*.c)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.gcov *.gcda *.gcno

# Show help
help:
	@echo "Available targets:"
	@echo "  all         - Build test executable (default)"
	@echo "  test        - Build and run main test suite"
	@echo "  test-all    - Build and run all tests"
	@echo "  memtest     - Run tests with valgrind memory checking"
	@echo "  quick       - Quick compile and run"
	@echo "  debug       - Build with debug symbols"
	@echo "  release     - Build optimized release version"
	@echo "  coverage    - Build with coverage and run tests"
	@echo "  coverage-report - Show coverage report"
	@echo "  analyze     - Run static analysis with cppcheck"
	@echo "  format      - Format code with clang-format"
	@echo "  clean       - Remove build artifacts"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Configuration:"
	@echo "  ZOBJ_DIR    - Directory containing zobj.c and zobj.h (default: ../src)"
	@echo "  CC          - Compiler to use (default: gcc)"
	@echo "  CFLAGS      - Compiler flags"
	@echo ""
	@echo "Example usage:"
	@echo "  make ZOBJ_DIR=/path/to/zobj/files test"
	@echo "  make CC=clang test"

# Create tests directory structure
setup:
	mkdir -p $(TEST_DIR)
	mkdir -p $(BUILD_DIR)
	@echo "Test directory structure created."
	@echo "Place additional test files in $(TEST_DIR)/ with naming pattern test_*.c"

# Install dependencies (Ubuntu/Debian)
install-deps:
	@echo "Installing dependencies..."
	sudo apt-get update
	sudo apt-get install -y gcc make valgrind cppcheck clang-format

# Phony targets
.PHONY: all test test-all memtest quick debug release coverage coverage-report analyze format clean help setup install-deps
