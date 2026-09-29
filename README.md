# ps5-stream-decompressor

Transfer large, highly compressible files (e.g. a 160 GB disk image living in a
35 GB `.7z`) from a PC to a PS5 **without extracting anything on the PC side**
and **without using any temporary storage on either side**.

The PC client compresses on the fly and streams over a single TCP connection;
the PS5 payload decodes each frame as it arrives and writes straight to
`/data/uploads`. If the input is a `.7z`, the client can pass the archive's own
LZMA2 stream through untouched, so the PS5 sees exactly the archive's
compressed bytes — the best possible traffic for the network.

```
        PC (client)                                 PS5 (server)
  ┌────────────────────┐   custom binary TCP   ┌──────────────────────┐
  │ file / zip / 7z    │ ────────────────────► │ ps5streamd.elf       │
  │  │                 │   HELLO + DATA frames │   │                  │
  │  ├─ 7z passthrough │                       │   ├─ LZMA1/2 decoder │
  │  │   raw LZMA2     │                       │   │   (7-Zip SDK)    │
  │  └─ zstd on the fly│                       │   └─ zstd decoder    │
  └────────────────────┘                       └──────────┬───────────┘
                                                          ▼
                                                  /data/uploads/<name>
```

## Why pass-through matters

`.7z` headers sit at the **end** of the file, so an archive cannot be
stream-decoded by a receiver that only sees a byte stream — the receiver would
have to buffer the whole archive first. That is why naive "upload the .7z"
servers do not work for this use case.

This project sidesteps the problem: the **client** has random access to the
local archive, so it parses the header with the 7-Zip SDK, locates the selected
member's packed stream, and forwards those bytes plus the coder properties.
The PS5 never sees a `.7z` container — only a raw LZMA1/LZMA2 stream it can
decode incrementally. Nothing is extracted or copied to disk on the PC.

When pass-through is not possible (zip archives, 7z members sharing a solid
block, unsupported coder chains), the client falls back to decoding the member
with libarchive and **recompressing it with zstd in a streaming fashion**.

## Design principles

* **No temporary files anywhere.**
  * Client: members are streamed from libarchive (or the 7z packed region) in
    fixed-size blocks straight into the encoder / socket.
  * Server: frames are decoded and written directly into the destination file.
    Resuming a zstd transfer only `ftruncate`s the existing partial file.
* **No buffering of the whole archive on the PS5.** The server never needs more
  than one frame.
* **Resumable where it is meaningful.** zstd is sent as independent frames, so a
  dropped transfer can be resumed at the last frame boundary. LZMA pass-through
  cannot expose mid-stream decoder state, so it stream-decodes and restarts from
  the beginning on a drop (no temporary storage is involved).
* **Safe by default.** Destination names are sanitised (basename only, no
  `/`, `..` or control characters) and always resolved under the destination
  directory. An optional shared token can gate connections.

## Layout

```
protocol/protocol.h     shared wire format
client/                 C CLI (ps5push): libzstd + libarchive + vendored 7-Zip
server/                 PS5 payload + portable core (also builds natively)
third_party/zstd/       vendored zstd (decompress-only use)
third_party/lzma/       7-Zip SDK decoder subset
tests/integration.sh    loopback end-to-end tests
```

## Wire protocol

All integers are little-endian. See `protocol/protocol.h` for the exact layout.

```
HELLO  magic "PS5S", version, codec, flags, name, sizes, chunk size,
       coder properties, resume offset, optional token
DATA   u32 comp_len | u32 raw_len | u8 flags | payload
       flags: 0 data, 1 final, 2 stored-raw, 3 abort
STATUS u8 type | u64 received | u64 written | u32 code   (server -> client)
```

A special `PS5SD_FLAG_QUERY` HELLO asks the server for the size of an existing
partial file; the client uses it to compute the resume offset before opening the
real transfer connection.

## Building

### Prerequisites

* Host: a C compiler, `make`, `pkg-config`, `libzstd` and `libarchive` with
  development headers.
* PS5: the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).

```sh
# Client + native (Linux) server for tests
make

# PS5 payload
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make server          # -> server/ps5streamd.elf
```

### Deploying to the PS5

Copy `server/ps5streamd.elf` to the console and launch it with an ELF loader
(for example `elfldr` or `websrv`). With the SDK installed you can deploy
directly:

```sh
cd server
PS5_HOST=192.168.1.50 PS5_PORT=9021 make test
```

The server listens on TCP port 9080 and writes to `/data/uploads` by default.

```
ps5streamd.elf [-p PORT] [-d DIR] [-t TOKEN] [-q]
```

## Usage

```sh
# Plain file, compressed on the fly
./client/ps5push -H 192.168.1.50 disk.img

# Pick a single member out of an archive (7z is passed through when possible)
./client/ps5push -H 192.168.1.50 disk.7z
./client/ps5push -H 192.168.1.50 -m sys/boot.img backups.zip

# Faster / smaller trade-offs
./client/ps5push -H 192.168.1.50 -l 9 -T 8 disk.img
./client/ps5push -H 192.168.1.50 -C 128M disk.img      # larger frames
./client/ps5push -H 192.168.1.50 -B 2G disk.img       # deeper send buffer
./client/ps5push -H 192.168.1.50 --raw already-compressed.bin

# Resume an interrupted zstd transfer
./client/ps5push -H 192.168.1.50 --resume disk.img

# Shared token
./client/ps5push -H 192.168.1.50 -t 0x1234 disk.7z   # server: -t 0x1234
```

Run `./client/ps5push --help` for all options.

### Tuning and parallelism

The zstd path splits the input into independent frames (default 8 MiB, `-C`)
and compresses them with a pool of worker threads (`-T`, default all cores).
Because the frames are independent, this is embarrassingly parallel — measured
~3× faster than a single-threaded encoder at level 19, and it scales across
physical cores. Independent frames also mean the same stream can be resumed at
any frame boundary.

A dedicated **sender thread** writes completed frames to the network in order,
so a slow or bursty link cannot stall the reader or the compressor: the network
is drained continuously while the CPU keeps working. The amount of data the
client may buffer ahead is bounded by `-B` (default 512 MiB); a larger buffer
helps on high-latency or intermittent links, a smaller one reduces memory.

`zstd` level (`-l`), window log (`-W`), frame size (`-C`, e.g. `8M`, `128M`,
`1G`) and buffer budget (`-B`) are configurable. Larger frames improve the ratio
(fewer independent frames reset the dictionary); smaller frames give finer
resume granularity and lower memory. Lower levels are dramatically faster.

Choose a frame size that leaves plenty of frames to go around: ideally
`file_size / chunk` should be several times the worker count, otherwise some
workers idle at the tail. For multi-GiB images, `64M`–`256M` is a good range.

For an already LZMA2-compressed `.7z`, pass-through is used regardless of these
settings and no recompression happens at all.

For an already LZMA2-compressed `.7z`, pass-through is used regardless of these
settings and no recompression happens at all.

### Measured performance

On a 16-thread machine, streaming a 330 MiB disk-image-like file over loopback:

| Path | Throughput |
|---|---|
| zstd level 19, 1 worker | ~46 MiB/s |
| zstd level 19, frame pool | ~150 MiB/s |
| zstd level 12, frame pool | ~745 MiB/s |
| raw (no compression) pipeline ceiling | ~1.2 GiB/s |
| 7z LZMA2 pass-through (no client CPU) | ~490 MiB/s |

The server decodes zstd at well over 1 GiB/s and LZMA2 at several hundred MiB/s,
so the server is network-bound and does not need a decoder pool. LZMA2
pass-through is a single continuous stream and is inherently serial anyway.

## Testing

```sh
make test          # or ./tests/integration.sh
```

The suite builds the client and the native server, generates fixtures (plain
files, zip, single/solid/delta 7z), then runs the full matrix over loopback:
plain zstd/raw, empty files, zip recompression, 7z pass-through, forced
recompression, solid-member selection, unsupported-filter rejection, token
auth, and zstd resume.

## Limitations

* 7z pass-through requires a member that occupies its own solid block and uses a
  single LZMA1/LZMA2 coder (the common case for a disk image). Other 7z layout
  falls back to libarchive + zstd.
* libarchive cannot decode every 7z filter chain (for example a delta-only
  archive). Such members are rejected cleanly rather than silently corrupted.
* LZMA pass-through transfers are not resumable mid-stream and restart from the
  beginning if the connection drops; this is the deliberate trade-off that keeps
  the server free of temporary storage.

## Third-party

* [facebook/zstd](https://github.com/facebook/zstd) — BSD-3-Clause.
* [7-Zip / LZMA SDK](https://www.7-zip.org/sdk.html) — public domain.
* [libarchive](https://libarchive.org/) — BSD.
