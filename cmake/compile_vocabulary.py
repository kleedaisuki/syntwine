"""Compile one upstream rank file into the portable Syntwine index format."""

import argparse
from base64 import b64decode
from pathlib import Path
from struct import Struct


MAGIC = b"SVOCAB01"
HEADER = Struct("<8sIIII")
SLOT = Struct("<Q")
RANK_MASK = (1 << 24) - 1
UINT32_MAX = (1 << 32) - 1
UINT64_MASK = (1 << 64) - 1
MIX = 0xC6A4A7935BD1E995
SEED = 0xC70F6907
SHIFT = 47


def hash_token(token: bytes) -> int:
    """MurmurHash64A over little-endian words, identical to the native reader."""
    h = SEED ^ ((len(token) * MIX) & UINT64_MASK)
    end = len(token) & ~7
    for start in range(0, end, 8):
        word = (int.from_bytes(token[start : start + 8], "little") * MIX) & UINT64_MASK
        word = ((word ^ (word >> SHIFT)) * MIX) & UINT64_MASK
        h = ((h ^ word) * MIX) & UINT64_MASK
    if end != len(token):
        tail = int.from_bytes(token[end:], "little")
        h = ((h ^ tail) * MIX) & UINT64_MASK
    h = ((h ^ (h >> SHIFT)) * MIX) & UINT64_MASK
    return h ^ (h >> SHIFT)


def read_ranks(source: Path) -> dict[bytes, int]:
    records: dict[bytes, int] = {}
    ranks: set[int] = set()
    with source.open("rb") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                encoded, rank_text = line.split()
                token = b64decode(encoded, validate=True)
                rank = int(rank_text)
                if not 0 < len(token) <= 255 or not 0 <= rank < RANK_MASK:
                    raise ValueError("token length or rank is outside the index format")
                if token in records or rank in ranks:
                    raise ValueError("duplicate token or rank")
            except ValueError as error:
                raise ValueError(f"{source}:{line_number}: {error}") from error
            records[token] = rank
            ranks.add(rank)
    if any(bytes([byte]) not in records for byte in range(256)):
        raise ValueError(f"{source}: vocabulary does not contain all 256 single bytes")
    return records


def compile_index(records: dict[bytes, int]) -> bytes:
    capacity = 1 << (2 * len(records) - 1).bit_length()
    slots = bytearray(capacity * SLOT.size)
    tokens = bytearray()
    for token, rank in records.items():
        pos = hash_token(token) & (capacity - 1)
        while SLOT.unpack_from(slots, pos * SLOT.size)[0]:
            pos = (pos + 1) & (capacity - 1)
        descriptor = (len(token) << 24) | (rank + 1)
        SLOT.pack_into(slots, pos * SLOT.size, (descriptor << 32) | len(tokens))
        tokens.extend(token)
    if len(tokens) > UINT32_MAX:
        raise ValueError("token byte section exceeds the index format")
    return HEADER.pack(MAGIC, capacity, len(records), max(map(len, records)), len(tokens)) + slots + tokens


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="upstream .tiktoken file")
    parser.add_argument("destination", type=Path, help="compiled .bin file")
    args = parser.parse_args()
    blob = compile_index(read_ranks(args.source))
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(blob)


if __name__ == "__main__":
    main()
