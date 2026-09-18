#!/usr/bin/env python3
"""The other coders over the same blocks.

    ./build/ambar_codec_bench <db-dir> --dump blocks.bin
    python3 tools/compare_codecs.py blocks.bin

Reads the blocks ambar_codec_bench dumped -- each a four-byte little-endian
length and its bytes -- and runs zlib (in every Python) and LZ4 (the `lz4`
package, if it is installed: `pip install lz4`) over them, reporting the same
ratio and rates ambar_codec_bench reports for the engine's own coder, so the
three can be read side by side in docs/BENCHMARKS.md.

The rates are those of the C libraries behind the bindings; the per-call
overhead of calling into them from Python is about a microsecond a block,
which on a four-kilobyte block at LZ4's speed is a few percent, and is
stated rather than corrected for.  The ratio has no such caveat.
"""
import struct
import sys
import time
import zlib


def read_blocks(path):
    data = open(path, "rb").read()
    blocks = []
    at = 0
    while at + 4 <= len(data):
        (n,) = struct.unpack_from("<I", data, at)
        at += 4
        blocks.append(data[at:at + n])
        at += n
    return blocks


def measure(name, compress, decompress, blocks, passes=5):
    raw = sum(len(b) for b in blocks)
    packed = [compress(b) for b in blocks]
    size = sum(len(p) for p in packed)
    shrink = sum(1 for b, p in zip(blocks, packed) if len(p) < len(b))
    best_c = best_d = float("inf")
    for _ in range(passes):
        t = time.perf_counter()
        for b in blocks:
            compress(b)
        best_c = min(best_c, time.perf_counter() - t)
        t = time.perf_counter()
        for p in packed:
            decompress(p)
        best_d = min(best_d, time.perf_counter() - t)
    for b, p in zip(blocks, packed):
        if decompress(p) != b:
            raise SystemExit(f"{name}: a block did not round-trip")
    mb = raw / 1048576.0
    print(f"{name:<18} {raw / size:5.2f}x  compress {mb / best_c:6.0f} MB/s"
          f"  decompress {mb / best_d:6.0f} MB/s  ({shrink} of {len(blocks)}"
          f" blocks would shrink)")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <blocks.bin>")
    blocks = read_blocks(sys.argv[1])
    if not blocks:
        raise SystemExit("no blocks in the dump")
    raw = sum(len(b) for b in blocks)
    print(f"{len(blocks)} blocks, {raw / 1048576.0:.1f} MB raw")

    for level in (1, 6):
        measure(f"zlib level {level}",
                lambda b, l=level: zlib.compress(b, l), zlib.decompress,
                blocks)
    try:
        import lz4.block
    except ImportError:
        print("lz4: not installed (pip install lz4); skipped")
        return
    # lz4.block prefixes the decoded size as four bytes, which stands in for
    # this engine's varint length; a fair match, and a byte or two either
    # way on a four-kilobyte block.
    measure("lz4 (default)", lz4.block.compress, lz4.block.decompress, blocks)
    measure("lz4 (high)",
            lambda b: lz4.block.compress(b, mode="high_compression"),
            lz4.block.decompress, blocks)


if __name__ == "__main__":
    main()
