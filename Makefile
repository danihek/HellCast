# hellcast

# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 `pkg-config --cflags playerctl glib-2.0`
LDFLAGS = -lm -lcurl -lncursesw `pkg-config --libs playerctl glib-2.0`

# Target
TARGET = hellcast
SRC = hellcast.c

# Default target
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

# Clean target
clean:
	rm -f $(TARGET)

# Rebuild target
rebuild: clean all

