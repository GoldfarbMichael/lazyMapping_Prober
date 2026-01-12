# Compiler
CC = gcc

# CFLAGS:
# -I./Mastik-Anatoly/src : Adds this folder to the search path for <angle_bracket> includes.
# It takes precedence over /usr/local/include.
CFLAGS = -g -O3 -Wall -Wextra -I./Mastik-Anatoly/src

# LDFLAGS:
# -L./Mastik-Anatoly/src : Tells the linker where to find libmastik.a
# -lmastik               : Links the library
LDFLAGS = -L./Mastik-Anatoly/src -lmastik

# Project Files
SRCS = src/main.c src/utils.c src/tests.c
OBJS = $(SRCS:.c=.o)

# Executable Name
TARGET = lazyMapping_Prober

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean