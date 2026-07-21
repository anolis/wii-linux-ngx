#!/usr/bin/env python3
"""Decode the GX state and FIFO frames stored in a Dolphin DFF capture."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


FILE_MAGIC = 0x0D01F1F0
HEADER = struct.Struct("<IIIQIQIQIQIQIIQIII8s24s")
FRAME = struct.Struct("<QIIIQI32s")

BP_NAMES = {
    0x00: "GEN_MODE",
    0x20: "SCISSOR_TL",
    0x21: "SCISSOR_BR",
    0x28: "TEV_ORDER_0_1",
    0x40: "Z_MODE",
    0x41: "BLEND_MODE",
    0x42: "DST_ALPHA",
    0x43: "PE_CONTROL",
    0x44: "FIELD_MASK",
    0x45: "PE_DONE",
    0x49: "COPY_SRC_TL",
    0x4A: "COPY_SRC_WH",
    0x4B: "COPY_DST_ADDR",
    0x4D: "COPY_DST_STRIDE",
    0x52: "COPY_CONTROL",
    0x59: "SCISSOR_OFFSET",
    0x68: "FIELD_MODE",
    0xC0: "TEV_COLOR_ENV_0",
    0xC1: "TEV_ALPHA_ENV_0",
    0xF3: "ALPHA_COMPARE",
    0xF6: "TEV_KSEL_0",
    0xF7: "TEV_KSEL_1",
}

CP_NAMES = {
    0x30: "MATRIX_INDEX_A",
    0x40: "MATRIX_INDEX_B",
    0x50: "VCD_LO",
    0x60: "VCD_HI",
    0x70: "VAT_A_0",
    0x80: "VAT_B_0",
    0x90: "VAT_C_0",
}

XF_NAMES = {
    0x1000: "ERROR",
    0x1005: "CLIP_DISABLE",
    0x1008: "VTX_SPEC",
    0x1009: "NUM_COLOR_CHANS",
    0x100C: "CHAN0_MAT_COLOR",
    0x100D: "CHAN1_MAT_COLOR",
    0x100E: "CHAN0_COLOR_CTRL",
    0x100F: "CHAN1_COLOR_CTRL",
    0x1010: "CHAN0_ALPHA_CTRL",
    0x1011: "CHAN1_ALPHA_CTRL",
    0x1012: "DUAL_TEX",
    0x1018: "MATRIX_INDEX_A",
    0x1019: "MATRIX_INDEX_B",
    0x101A: "VIEWPORT_SCALE_X",
    0x101B: "VIEWPORT_SCALE_Y",
    0x101C: "VIEWPORT_SCALE_Z",
    0x101D: "VIEWPORT_OFFSET_X",
    0x101E: "VIEWPORT_OFFSET_Y",
    0x101F: "VIEWPORT_OFFSET_Z",
    0x1020: "PROJECTION_A",
    0x1021: "PROJECTION_B",
    0x1022: "PROJECTION_C",
    0x1023: "PROJECTION_D",
    0x1024: "PROJECTION_E",
    0x1025: "PROJECTION_F",
    0x1026: "PROJECTION_TYPE",
    0x103F: "NUM_TEX_GENS",
}

PRIMITIVES = {
    0: "QUADS",
    1: "QUADS_2",
    2: "TRIANGLES",
    3: "TRIANGLE_STRIP",
    4: "TRIANGLE_FAN",
    5: "LINES",
    6: "LINE_STRIP",
    7: "POINTS",
}


def read_u32_array(data: bytes, offset: int, count: int) -> list[int]:
    return list(struct.unpack_from(f"<{count}I", data, offset))


def component_size(fmt: int) -> int:
    return (1, 1, 2, 2, 4)[fmt]


def vertex_size(cp: list[int], vat: int) -> int:
    """Calculate vertex bytes from Dolphin's TVtxDesc/UVAT bit layout."""
    lo = cp[0x50]
    hi = cp[0x60]
    va = cp[0x70 + vat]
    vb = cp[0x80 + vat]
    vc = cp[0x90 + vat]
    size = (lo & 0x1FF).bit_count()

    def attr(desc: int, direct_size: int) -> int:
        if desc == 0:
            return 0
        if desc == 1:
            return direct_size
        return 1 if desc == 2 else 2

    pos_components = 2 + ((va >> 0) & 1)
    pos_format = (va >> 1) & 7
    size += attr((lo >> 9) & 3, pos_components * component_size(pos_format))

    normal_desc = (lo >> 11) & 3
    if normal_desc == 1:
        normal_components = 3 * (1 + 2 * ((va >> 9) & 1))
        size += normal_components * component_size((va >> 10) & 7)
    elif normal_desc:
        index_count = 3 if ((va >> 31) & 1) and ((va >> 9) & 1) else 1
        size += index_count * (1 if normal_desc == 2 else 2)

    color_sizes = (2, 3, 4, 2, 3, 4)
    size += attr((lo >> 13) & 3, color_sizes[(va >> 14) & 7])
    size += attr((lo >> 15) & 3, color_sizes[(va >> 18) & 7])

    tex_fields = [
        ((va >> 21) & 1, (va >> 22) & 7),
        ((vb >> 0) & 1, (vb >> 1) & 7),
        ((vb >> 9) & 1, (vb >> 10) & 7),
        ((vb >> 18) & 1, (vb >> 19) & 7),
        ((vb >> 27) & 1, (vb >> 28) & 7),
        ((vc >> 5) & 1, (vc >> 6) & 7),
        ((vc >> 14) & 1, (vc >> 15) & 7),
        ((vc >> 23) & 1, (vc >> 24) & 7),
    ]
    for index, (elements, fmt) in enumerate(tex_fields):
        size += attr((hi >> (index * 2)) & 3, (1 + elements) * component_size(fmt))
    return size


def describe_vertex(payload: bytes, cp: list[int], vat: int) -> str:
    lo = cp[0x50]
    va = cp[0x70 + vat]
    if ((lo >> 9) & 3) != 1 or ((va >> 1) & 7) != 4:
        return payload.hex()

    pos_count = 2 + (va & 1)
    pos_bytes = pos_count * 4
    values = struct.unpack_from(">" + "f" * pos_count, payload)
    fields = ["pos=(" + ", ".join(f"{value:g}" for value in values) + ")"]
    color_desc = (lo >> 13) & 3
    color_format = (va >> 14) & 7
    if color_desc == 1 and color_format == 5:
        fields.append("rgba=" + payload[pos_bytes : pos_bytes + 4].hex())
    return " ".join(fields)


def decode_fifo(data: bytes, cp_initial: list[int]) -> list[str]:
    cp = cp_initial.copy()
    output: list[str] = []
    offset = 0
    while offset < len(data):
        opcode = data[offset]
        if opcode == 0x00:
            end = offset + 1
            while end < len(data) and data[end] == 0:
                end += 1
            output.append(f"{offset:04x}: NOP x{end - offset}")
            offset = end
        elif opcode == 0x08:
            reg = data[offset + 1]
            value = struct.unpack_from(">I", data, offset + 2)[0]
            cp[reg] = value
            output.append(
                f"{offset:04x}: CP {reg:02x} {value:08x} {CP_NAMES.get(reg, '')}".rstrip()
            )
            offset += 6
        elif opcode == 0x10:
            header = struct.unpack_from(">I", data, offset + 1)[0]
            count = ((header >> 16) & 0xF) + 1
            address = header & 0xFFFF
            output.append(f"{offset:04x}: XF {address:04x} count={count}")
            for index in range(count):
                value = struct.unpack_from(">I", data, offset + 5 + index * 4)[0]
                reg = address + index
                output.append(
                    f"      {reg:04x} {value:08x} {XF_NAMES.get(reg, '')}".rstrip()
                )
            offset += 5 + count * 4
        elif opcode == 0x61:
            reg = data[offset + 1]
            value = int.from_bytes(data[offset + 2 : offset + 5], "big")
            output.append(
                f"{offset:04x}: BP {reg:02x} {value:06x} {BP_NAMES.get(reg, '')}".rstrip()
            )
            offset += 5
        elif 0x80 <= opcode <= 0xBF:
            primitive = (opcode & 0x38) >> 3
            vat = opcode & 7
            count = struct.unpack_from(">H", data, offset + 1)[0]
            stride = vertex_size(cp, vat)
            output.append(
                f"{offset:04x}: DRAW {PRIMITIVES[primitive]} vat={vat} "
                f"vertices={count} stride={stride}"
            )
            start = offset + 3
            for index in range(count):
                vertex = data[start + index * stride : start + (index + 1) * stride]
                output.append(f"      v{index}: {describe_vertex(vertex, cp, vat)}")
            offset = start + count * stride
        else:
            raise ValueError(f"unknown opcode 0x{opcode:02x} at FIFO offset 0x{offset:x}")
    return output


def print_state(title: str, values: list[int], names: dict[int, str], base: int = 0) -> None:
    print(title)
    for register, name in names.items():
        index = register - base
        print(f"  {register:04x} {values[index]:08x} {name}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()

    data = args.capture.read_bytes()
    fields = HEADER.unpack_from(data)
    if fields[0] != FILE_MAGIC:
        raise ValueError(f"bad DFF magic 0x{fields[0]:08x}")

    bp = read_u32_array(data, fields[3], fields[4])
    cp = read_u32_array(data, fields[5], fields[6])
    xf_memory = read_u32_array(data, fields[7], fields[8])
    xf_registers = read_u32_array(data, fields[9], fields[10])
    frame_list_offset = fields[11]
    frame_count = fields[12]

    print(f"file: {args.capture}")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    print(f"version: {fields[1]} minimum_loader: {fields[2]} frames: {frame_count}")
    print_state("initial BP state:", bp, BP_NAMES)
    print_state("initial CP state:", cp, CP_NAMES)
    print_state("initial XF registers:", xf_registers, XF_NAMES, 0x1000)
    print("initial position matrix 0:")
    for index, value in enumerate(xf_memory[:12]):
        print(f"  {index:04x} {value:08x}")

    for frame_index in range(frame_count):
        frame = FRAME.unpack_from(data, frame_list_offset + frame_index * FRAME.size)
        fifo_offset, fifo_size, fifo_start, fifo_end, updates_offset, updates_count, _ = frame
        fifo = data[fifo_offset : fifo_offset + fifo_size]
        print(
            f"frame {frame_index}: fifo_bytes={fifo_size} start=0x{fifo_start:08x} "
            f"end=0x{fifo_end:08x} memory_updates={updates_count} "
            f"updates_offset=0x{updates_offset:x}"
        )
        for line in decode_fifo(fifo, cp):
            print("  " + line)


if __name__ == "__main__":
    main()
