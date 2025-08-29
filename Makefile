CC ?= gcc
CFLAGS := -Wall -Wextra -O2
CFLAGS += -Wno-unused-function -Wno-format-truncation
PREFIX ?= /usr/local

BIN_PROGS = sidecar

all: $(BIN_PROGS)

sidecar: sidecar.c
	$(CC) $(CFLAGS) sidecar.c -o $@

clean:
	rm -f $(BIN_PROGS)


