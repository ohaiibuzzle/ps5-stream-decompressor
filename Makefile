# ps5-stream-decompressor -- convenience targets.
#
#   make            build the client and the native (test) server
#   make server     build the PS5 payload (requires PS5_PAYLOAD_SDK)
#   make test       build and run the loopback integration tests
#   make clean

.PHONY: all client server native test clean

all: client native

client:
	$(MAKE) -C client

native:
	$(MAKE) -C server -f Makefile.host

server:
	$(MAKE) -C server

test:
	./tests/integration.sh

clean:
	$(MAKE) -C client clean
	$(MAKE) -C server -f Makefile.host clean
	$(MAKE) -C server clean
