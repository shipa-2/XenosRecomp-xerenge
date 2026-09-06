#!/usr/bin/env python3
"""Wrap captured Xenos vertex microcode in a container XenosRecomp can scan."""

import argparse
import struct
from pathlib import Path


USAGES = {
    "position": 0,
    "blendweight": 1,
    "blendindices": 2,
    "normal": 3,
    "pointsize": 4,
    "texcoord": 5,
    "tangent": 6,
    "binormal": 7,
    "color": 10,
}


def declaration(value: str) -> tuple[int, int, int]:
    try:
        address, usage, index = value.split(":")
        return int(address, 0), USAGES[usage.lower()], int(index, 0)
    except (ValueError, KeyError) as error:
        raise argparse.ArgumentTypeError(
            "expected ADDRESS:USAGE:INDEX, for example 3:position:0"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("microcode", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--element", action="append", type=declaration, required=True)
    parser.add_argument("--interpolator", action="append", type=declaration, required=True)
    args = parser.parse_args()

    code = args.microcode.read_bytes()
    if not code or len(code) % 4:
        parser.error("microcode must contain big-endian 32-bit words")

    shader_offset = 0x5C
    array_offset = shader_offset + 0x24
    virtual_size = array_offset + 4 * (len(args.element) + len(args.interpolator))
    data = bytearray(virtual_size + len(code))

    def word(offset: int, value: int) -> None:
        struct.pack_into(">I", data, offset, value)

    def half(offset: int, value: int) -> None:
        struct.pack_into(">H", data, offset, value)

    header = [0x102A1101, virtual_size, len(code), 0, 0x24, 0,
              shader_offset, 0, 0]
    for index, value in enumerate(header):
        word(index * 4, value)

    # One generic c[0..255] declaration keeps raw runtime shaders independent
    # of a D3DX constant table while preserving every constant-register read.
    word(0x24, 56)
    for index, value in enumerate([28, 0, 0, 1, 28, 0, 0]):
        word(0x28 + index * 4, value)
    constant = 0x44
    word(constant, 48)
    half(constant + 4, 2)  # RegisterSet::Float4
    half(constant + 6, 0)
    half(constant + 8, 256)
    data[0x58:0x5A] = b"c\0"

    shader = [0, len(code), 0, 0, 0, len(args.interpolator) << 5,
              0, len(args.element), 0]
    for index, value in enumerate(shader):
        word(shader_offset + index * 4, value)

    for index, (address, usage, usage_index) in enumerate(args.element):
        word(array_offset + index * 4,
             address | (usage << 12) | (usage_index << 16))
    interpolator_offset = array_offset + len(args.element) * 4
    for index, (usage_index, usage, register) in enumerate(args.interpolator):
        word(interpolator_offset + index * 4,
             usage_index | (usage << 4) | (register << 8))

    data[virtual_size:] = code
    args.output.write_bytes(data)


if __name__ == "__main__":
    main()
