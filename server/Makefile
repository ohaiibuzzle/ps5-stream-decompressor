# Makefile -- PS5 payload build (ps5-payload-dev SDK).
PS5_HOST ?= ps5
PS5_PORT ?= 9021

# The SDK is only required to build/deploy the payload, not to clean.
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

ELF     := ps5streamd.elf
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZSTD_DISABLE_ASM
CPPFLAGS += -I../protocol \
            -I../third_party/zstd \
            -I../third_party/zstd/common \
            -I../third_party/zstd/decompress \
            -I../third_party/lzma

SERVER_SRC := main-prospero.c stream_server.c io.c decode.c decode_zstd.c \
              decode_lzma.c
ZSTD_SRC   := $(wildcard ../third_party/zstd/common/*.c) \
              $(wildcard ../third_party/zstd/decompress/*.c)
LZMA_SRC   := ../third_party/lzma/LzmaDec.c \
              ../third_party/lzma/Lzma2Dec.c \
              ../third_party/lzma/CpuArch.c

all: $(ELF)

$(ELF): $(SERVER_SRC) $(ZSTD_SRC) $(LZMA_SRC)
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined; see the README" >&2; exit 1; }
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean test
