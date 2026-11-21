# Tokenizer vocabulary provenance

The source rank files in `python/syntwine/tokenizer/` are produced by OpenAI and kept unmodified. See the
[upstream encoding descriptors](https://github.com/openai/tiktoken/blob/main/tiktoken_ext/openai_public.py)
and included MIT license. The checksums match those listed by the upstream tokenizer.

| Asset | SHA-256 |
|---|---|
| [cl100k_base](https://openaipublic.blob.core.windows.net/encodings/cl100k_base.tiktoken) | `223921b76ee99bde995b7ff738513eef100fb51d18c93597a113bcffe865b2a7` |
| [o200k_base](https://openaipublic.blob.core.windows.net/encodings/o200k_base.tiktoken) | `446a9538cb6c348e3516120d7c08b09f57c36495e2acfffe59a5bf8b0cfb1a2d` |
| [p50k_base](https://openaipublic.blob.core.windows.net/encodings/p50k_base.tiktoken) | `94b5ca7dff4d00767bc256fdd1b27e5b17361d7b8a5f968547f9f23eb70d2069` |
| [r50k_base](https://openaipublic.blob.core.windows.net/encodings/r50k_base.tiktoken) | `306cd27f03c1a714eca7108e03d66b7dc042abe8c258b44c199a7ed9838dd930` |

## Compiled index

CMake runs `cmake/compile_vocabulary.py` on the source rank files. The source
distribution carries those inputs; wheels carry the generated indexes and
the vocabulary license, not both copies of the data. The native extension
reads one complete index on the first use of the corresponding encoding;
other encodings remain unloaded. No mapping or runtime compilation is needed.

Each file starts with the eight-byte magic/version `SVOCAB01`, followed by
four little-endian 32-bit fields: slot count, occupied slot count, maximum
key length, and key bytes. An array of little-endian 64-bit slots follows,
then the concatenated token bytes. A nonzero slot stores the byte offset in
its low 32 bits, rank plus one in the next 24 bits, and length in its high
eight bits. Zero is an empty slot. Ranks are never renumbered; some upstream
tables, such as `p50k_base`, have gaps.

The format fixes [MurmurHash64A](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp)
over little-endian words with seed `0xc70f6907` and uses linear probing.
The C++ reader validates the dimensions and slots once before lookup and
checks candidate token bytes for exact equality. The format is private to
Syntwine; change the version together with the compiler and reader if the
on-disk layout or hash changes.
