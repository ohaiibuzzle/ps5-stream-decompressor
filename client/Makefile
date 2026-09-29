# Makefile -- ps5push client (host build).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread
PKGS    := libzstd libarchive
CPPFLAGS += -I../protocol -I../third_party/lzma \
            $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -pthread

SRC      := main.c transport.c compress.c archive.c sevenzip.c
LZMA_SRC := $(wildcard ../third_party/lzma/*.c)

BIN := ps5push

all: $(BIN)

$(BIN): $(SRC) $(LZMA_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
