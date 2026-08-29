# Compiler
CC = gcc

# PATHS
# Relative path to your stable Mastik library
MASTIK_DIR = ./Mastik-main

# CFLAGS:
# -I$(MASTIK_DIR)/src : Adds your local Mastik src folder to the include path.
# -O0                  : Retained for wrapper debugging (Warning: Increases L1 cache noise).
# -std=gnu99           : Required for inline functions, stdint.h, and inline assembly extensions.
# -fno-strict-aliasing : Required because Murmur3 uses block pointer casting that violates strict aliasing.
CFLAGS = -g -O0 -Wall -Wno-implicit-fallthrough -Wextra -std=gnu99 -fno-strict-aliasing -I$(MASTIK_DIR)/src -I$(MASTIK_DIR)

# LDFLAGS:
# -L$(MASTIK_DIR)/src : Tells the linker to look in your local folder for libmastik.a
# -lmastik            : Links the library
LDFLAGS = -L$(MASTIK_DIR)/src -lmastik -lm

# Project Files
# Assuming you moved your code into a 'src' folder inside 'stable'
SRCS = src/main.c src/utils.c src/tests.c src/mastikElite.c src/murmur3.c src/lazy_map.c
OBJS = $(SRCS:.c=.o)

# Executable Name
TARGET = MastikElite

# Coverage validator (cross-process P+P against the JS lazy mapping).
# Reuses every non-main object (utils/tests/mastikElite/murmur3) + its own main.
COV_TARGET = CoverageValidator
COV_SRCS = src/coverage_validator.c src/utils.c src/tests.c src/mastikElite.c src/murmur3.c src/lazy_map.c src/pmu.c
COV_OBJS = $(COV_SRCS:.c=.o)

# Fingerprint orchestrator (real-browser Stage 3: drives Chrome JS sampling + stress-ng).
# Reuses the same non-main objects + its own main.
FP_TARGET = FingerprintOrchestrator
FP_SRCS = src/fingerprint_orchestrator.c src/fp_ctl.c src/utils.c src/tests.c src/mastikElite.c src/murmur3.c src/lazy_map.c
FP_OBJS = $(FP_SRCS:.c=.o)

# Website orchestrator (Stage 4: drives Chrome JS sampling + real websites in a second tab).
# Same object set as FP_SRCS (it needs mastikElite.c's parse_*_from_dirname) + its own main.
WEB_TARGET = WebsiteOrchestrator
WEB_SRCS = src/website_orchestrator.c src/fp_ctl.c src/utils.c src/tests.c src/mastikElite.c src/murmur3.c src/lazy_map.c
WEB_OBJS = $(WEB_SRCS:.c=.o)

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

$(COV_TARGET): $(COV_OBJS)
	$(CC) $(CFLAGS) -o $(COV_TARGET) $(COV_OBJS) $(LDFLAGS)

$(FP_TARGET): $(FP_OBJS)
	$(CC) $(CFLAGS) -o $(FP_TARGET) $(FP_OBJS) $(LDFLAGS)

$(WEB_TARGET): $(WEB_OBJS)
	$(CC) $(CFLAGS) -o $(WEB_TARGET) $(WEB_OBJS) $(LDFLAGS)

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Helper to build Mastik itself if you haven't yet
# Usage: make mastik
mastik:
	cd $(MASTIK_DIR)/src && $(MAKE)

clean:
	rm -f $(OBJS) $(COV_OBJS) $(FP_OBJS) $(WEB_OBJS) $(TARGET) $(COV_TARGET) $(FP_TARGET) $(WEB_TARGET)

.PHONY: all clean mastik