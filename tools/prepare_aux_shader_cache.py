#!/usr/bin/env python3
"""Rename a generated XenosRecomp cache for auxiliary runtime linking.

The runtime's main cache keeps the normal XenosRecomp symbols. This tool makes
a second generated cache link beside it without copying shader blobs into the
source tree or changing the generator's output.
"""

import argparse
from pathlib import Path


SYMBOLS = (
    "g_shaderCacheEntries",
    "g_shaderCacheEntryCount",
    "g_shaderMicrocodeEntries",
    "g_shaderMicrocodeEntryCount",
    "g_compressedSpirvCache",
    "g_spirvCacheCompressedSize",
    "g_spirvCacheDecompressedSize",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="XenosRecomp generated cache .cpp")
    parser.add_argument("output", type=Path, help="auxiliary cache .cpp")
    args = parser.parse_args()
    source = args.input.read_text()
    for symbol in SYMBOLS:
        source = source.replace(symbol, symbol + "Extra")

    # A generated file declares these sizes through the normal header. The
    # renamed file has no matching header declaration, so make the definitions
    # externally visible for the runtime's weak optional imports.
    for symbol in (
        "g_shaderCacheEntryCountExtra",
        "g_shaderMicrocodeEntryCountExtra",
        "g_spirvCacheCompressedSizeExtra",
        "g_spirvCacheDecompressedSizeExtra",
    ):
        source = source.replace("const size_t " + symbol,
                              "extern const size_t " + symbol)
    source = source.replace("const uint8_t g_compressedSpirvCacheExtra[]",
                            "extern const uint8_t g_compressedSpirvCacheExtra[]")
    args.output.write_text(source)


if __name__ == "__main__":
    main()
