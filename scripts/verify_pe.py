#!/usr/bin/env python3
"""
Verify that a cross-compiled tessera.exe is what we claim it is.

Cross-compiling proves the code compiles for Windows; this checks the resulting
image actually has the properties the README promises:

  * it is a real PE32+ (64-bit) image with a console subsystem;
  * it does not depend on any DLL outside the Windows system set, so it needs no
    Visual C++ redistributable or side-by-side files.

Exit status is 0 when every check passes, 1 otherwise. No third-party modules
are required, which matters because this runs in CI before anything is installed.

    python3 scripts/verify_pe.py dist/tessera.exe
"""
from __future__ import annotations

import struct
import sys

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
PE32_PLUS_MAGIC = 0x20B
CONSOLE_SUBSYSTEM = 3

# DLLs that ship with Windows itself (or come in with the Universal C Runtime
# that is part of Windows 10+). Anything else means the executable is not
# self-contained and the README's claim is false.
ALLOWED_IMPORT_PREFIXES = (
    "KERNEL32",
    "KERNELBASE",
    "SHELL32",
    "ADVAPI32",
    "USER32",
    "OLE32",
    "NTDLL",
    "api-ms-win-",
    "ext-ms-",
)


def read_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_sections(data: bytes, pe_offset: int) -> list[tuple[int, int, int]]:
    section_count = read_u16(data, pe_offset + 6)
    optional_size = read_u16(data, pe_offset + 20)
    table = pe_offset + 24 + optional_size
    sections = []
    for index in range(section_count):
        entry = table + 40 * index
        name = data[entry : entry + 8].rstrip(b"\x00").decode("ascii", "replace")
        virtual_size = read_u32(data, entry + 8)
        virtual_address = read_u32(data, entry + 12)
        raw_size = read_u32(data, entry + 16)
        raw_offset = read_u32(data, entry + 20)
        sections.append((name, virtual_address, max(virtual_size, raw_size), raw_offset))
    return sections


def rva_to_offset(sections: list[tuple[int, int, int, int]], rva: int) -> int | None:
    for _, virtual_address, size, raw_offset in sections:
        if virtual_address <= rva < virtual_address + size:
            return raw_offset + (rva - virtual_address)
    return None


def imported_dlls(data: bytes, pe_offset: int, sections, directories_offset: int) -> list[str]:
    import_rva = read_u32(data, directories_offset + 8)
    if import_rva == 0:
        return []
    offset = rva_to_offset(sections, import_rva)
    if offset is None:
        raise SystemExit("import directory RVA does not map to any section")

    names: list[str] = []
    while True:
        entry = data[offset : offset + 20]
        if len(entry) < 20 or entry == b"\x00" * 20:
            break
        name_rva = read_u32(data, offset + 12)
        if name_rva == 0:
            break
        name_offset = rva_to_offset(sections, name_rva)
        if name_offset is None:
            raise SystemExit("import name RVA does not map to any section")
        end = data.index(b"\x00", name_offset)
        names.append(data[name_offset:end].decode("ascii", "replace"))
        offset += 20
    return names


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2 if len(argv) != 2 else 0

    path = argv[1]
    with open(path, "rb") as handle:
        data = handle.read()

    failures: list[str] = []

    if data[:2] != b"MZ":
        print(f"FAIL  {path} is not an MS-DOS/PE image (missing 'MZ')")
        return 1

    pe_offset = read_u32(data, 0x3C)
    if data[pe_offset : pe_offset + 4] != b"PE\x00\x00":
        print(f"FAIL  {path} has no PE signature at 0x{pe_offset:x}")
        return 1

    machine = read_u16(data, pe_offset + 4)
    optional_offset = pe_offset + 24
    magic = read_u16(data, optional_offset)

    print(f"file          {path} ({len(data):,} bytes)")
    print(f"machine       {hex(machine)}")
    print(f"format        {'PE32+' if magic == PE32_PLUS_MAGIC else hex(magic)}")

    if magic != PE32_PLUS_MAGIC:
        failures.append(f"expected a PE32+ (64-bit) image, found magic {hex(magic)}")
    if machine not in (IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64):
        failures.append(f"expected x86-64 or ARM64, found machine {hex(machine)}")

    subsystem = read_u16(data, optional_offset + 68)
    print(f"subsystem     {subsystem} ({'console' if subsystem == CONSOLE_SUBSYSTEM else 'other'})")
    if subsystem != CONSOLE_SUBSYSTEM:
        failures.append(f"expected the console subsystem (3), found {subsystem}")

    sections = parse_sections(data, pe_offset)
    directories_offset = optional_offset + (112 if magic == PE32_PLUS_MAGIC else 96)
    dlls = imported_dlls(data, pe_offset, sections, directories_offset)

    print(f"imports       {', '.join(dlls) if dlls else '(none)'}")
    unexpected = [dll for dll in dlls if not dll.startswith(ALLOWED_IMPORT_PREFIXES)]
    if unexpected:
        failures.append(
            "non-system imports found — the executable is not self-contained: "
            + ", ".join(unexpected)
        )

    # A rasterised hint that we are looking at the right program, not a stub.
    if b"tessera" not in data:
        failures.append("the image contains no 'tessera' string; is this the right binary?")

    if failures:
        print()
        for failure in failures:
            print(f"FAIL  {failure}")
        return 1

    print()
    print("OK    static PE32+ console executable importing only Windows system libraries")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
