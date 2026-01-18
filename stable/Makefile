# Compiler
CC = gcc

# PATHS
# Relative path to your stable Mastik library
MASTIK_DIR = ./Mastik-main

# CFLAGS:
# -I$(MASTIK_DIR)/src : Adds your local Mastik src folder to the include path.
CFLAGS = -g -O3 -Wall -Wextra -I$(MASTIK_DIR)/src -I$(MASTIK_DIR)

# LDFLAGS:
# -L$(MASTIK_DIR)/src : Tells the linker to look in your local folder for libmastik.a
# -lmastik            : Links the library
LDFLAGS = -L$(MASTIK_DIR)/src -lmastik

# Project Files
# Assuming you moved your code into a 'src' folder inside 'stable'
SRCS = src/main.c src/utils.c src/tests.c
OBJS = $(SRCS:.c=.o)

# Executable Name
TARGET = MastikElite

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Helper to build Mastik itself if you haven't yet
# Usage: make mastik
mastik:
	cd $(MASTIK_DIR)/src && $(MAKE)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean mastik