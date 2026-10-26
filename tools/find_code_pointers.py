#!/usr/bin/env python3
"""Lists the data words of the ELF that point into .text at an address the recompiler does not dispatch: vtables,
callback tables and function pointers Ghidra did not turn into functions. A target qualifies when it opens a
function: a stack adjustment (addiu sp, sp, -n) or, for a leaf, a target that follows a return (jr ra + delay slot).

Usage: tools/find_code_pointers.py <elf> <generated/register_functions.cpp>   -> TOML entry_points strings."""
import re
import struct
import sys

elf_path, register_path = sys.argv[1], sys.argv[2]
data = open(elf_path, "rb").read()

shoff, = struct.unpack_from("<I", data, 32)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
sections = []
for i in range(shnum):
    name, stype, flags, addr, off, size = struct.unpack_from("<6I", data, shoff + i * shentsize)
    sections.append([name, stype, flags, addr, off, size])
strtab_off = sections[shstrndx][4]
def sname(s):
    end = data.index(b"\0", strtab_off + s[0])
    return data[strtab_off + s[0]:end].decode()

text = next(s for s in sections if sname(s) == ".text")
text_va, text_off, text_size = text[3], text[4], text[5]
def word(va):
    return struct.unpack_from("<I", data, text_off + va - text_va)[0]

registered = set(int(a, 16) for a in re.findall(r"// 0x([0-9a-fA-F]+)\s*$", open(register_path).read(), re.M))

candidates = {}
for s in sections:
    if sname(s) not in (".data", ".rodata", ".sdata") or s[1] != 1:
        continue
    for off in range(0, s[5] - 3, 4):
        target, = struct.unpack_from("<I", data, s[4] + off)
        if text_va <= target < text_va + text_size and target % 4 == 0 and target not in registered:
            candidates.setdefault(target, []).append(s[3] + off)

entries = []
for target, refs in sorted(candidates.items()):
    w = word(target)
    opens_frame = (w & 0xFFFF0000) == 0x27BD0000 and (w & 0x8000)  # addiu sp, sp, -n
    after_return = target - 8 >= text_va and word(target - 8) == 0x03E00008  # jr ra; delay slot; target
    if opens_frame or after_return:
        entries.append((target, refs))

for target, refs in entries:
    print(f'  "0x{target:08X}", # {len(refs)} ref(s), first at 0x{refs[0]:08X}')
print(f"# {len(entries)} entry points from {len(candidates)} undispatched code pointers", file=sys.stderr)
