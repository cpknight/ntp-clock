# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lm

# Target executable
TARGET = ntp-clock 

# Source files and object files
SRCS = ntp_client.c clock_display.c
OBJS = $(SRCS:.c=.o)

# Default target
.PHONY: all clean

all: build

build: $(TARGET)

# Link the target executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Dependencies
ntp_client.o: ntp_client.c ntp_client.h
clock_display.o: clock_display.c

# Clean target
clean:
	rm -f $(TARGET) $(OBJS) *~

