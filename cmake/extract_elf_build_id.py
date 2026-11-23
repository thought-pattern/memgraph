#!/usr/bin/env python3
"""Extract the hexadecimal GNU build-id from an ELF file's PT_NOTE / SHT_NOTE sections."""

from pathlib import Path
from struct import unpack_from
from sys import argv, exit as sys_exit


def read_elf_gnu_build_id(elf_path: Path) -> str:
    """Return lowercase hex GNU build-id from elf_path, or empty string if absent."""
    data = elf_path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return ""
    is_64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is_64:
        if len(data) < 64:
            return ""
        e_shoff = unpack_from(endian + "Q", data, 40)[0]
        e_shentsize = unpack_from(endian + "H", data, 58)[0]
        e_shnum = unpack_from(endian + "H", data, 60)[0]
    else:
        e_shoff = unpack_from(endian + "I", data, 32)[0]
        e_shentsize = unpack_from(endian + "H", data, 46)[0]
        e_shnum = unpack_from(endian + "H", data, 48)[0]

    for idx in range(e_shnum):
        base = e_shoff + idx * e_shentsize
        if base + e_shentsize > len(data):
            break
        sh_type = unpack_from(endian + "I", data, base + 4)[0]
        if sh_type != 7:
            continue
        if is_64:
            sh_offset = unpack_from(endian + "Q", data, base + 24)[0]
            sh_size = unpack_from(endian + "Q", data, base + 32)[0]
        else:
            sh_offset = unpack_from(endian + "I", data, base + 16)[0]
            sh_size = unpack_from(endian + "I", data, base + 20)[0]
        pos = sh_offset
        end = min(len(data), sh_offset + sh_size)
        while pos + 12 <= end:
            namesz, descsz, note_type = unpack_from(endian + "III", data, pos)
            pos += 12
            name_bytes = data[pos : pos + namesz]
            pos += (namesz + 3) & ~3
            desc_bytes = data[pos : pos + descsz]
            pos += (descsz + 3) & ~3
            if note_type == 3 and name_bytes.rstrip(b"\x00") == b"GNU" and desc_bytes:
                found_hex = desc_bytes.hex().lower()
                return found_hex
    return ""


def main() -> int:
    """CLI entrypoint printing the ELF GNU build-id to stdout."""
    if len(argv) < 2:
        return 1
    target = Path(argv[1])
    if not target.is_file():
        return 1
    bid = read_elf_gnu_build_id(target)
    if not bid:
        return 1
    print(bid)
    return 0


if __name__ == "__main__":
    sys_exit(main())
