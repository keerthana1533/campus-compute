CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=gnu11 -pthread
LDLIBS  = -lrt
TARGET  = campuscompute
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:src/%.c=build/%.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/common.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

# removes only files this Makefile created
clean:
	rm -rf build $(TARGET)

.PHONY: all clean
