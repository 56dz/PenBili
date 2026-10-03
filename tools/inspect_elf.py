"""检查交叉编译出的 ARM .so：架构、ABI、依赖、glibc 版本需求、导出符号。"""
import struct
import sys

if len(sys.argv) > 1:
    PATH = sys.argv[1]
else:
    print("usage: inspect_elf.py <elf-path>")
    sys.exit(2)

with open(PATH, "rb") as fh:
    data = fh.read()

if data[:4] != b"\x7fELF":
    print("not ELF")
    sys.exit(1)
ei_class = data[4]
ei_data = data[5]
assert ei_class == 1, "expected ELF32, got class %d" % ei_class
assert ei_data == 1, "expected little endian"

(e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)

MACHINES = {40: "ARM", 3: "x86", 62: "x86-64", 183: "AArch64"}
print("machine = %s (%d)" % (MACHINES.get(e_machine, "?"), e_machine))
print("type    = %d (3=DYN/shared)" % e_type)
print("flags   = 0x%x" % e_flags)
abi = (e_flags >> 24) & 0xFF
print("        = EABI version %d (5= EABI5), hard-float flag=%s" % (abi, bool(e_flags & 0x400)))

# section headers
shdrs = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    shdrs.append(struct.unpack_from("<IIIIIIIIII", data, off))
shstr = shdrs[e_shstrndx]
def shname(entry):
    o = shstr[4] + entry[0]
    end = data.index(b"\0", o)
    return data[o:end].decode("utf-8", "replace")

sections = {}
for i, sh in enumerate(shdrs):
    sections[shname(sh)] = (i, sh)

# dynamic section -> NEEDED / SONAME
dyn_idx, dyn_sh = sections[".dynamic"]
dyn_entries = []
off = dyn_sh[4]
while True:
    tag, val = struct.unpack_from("<iI", data, off)
    off += 8
    if tag == 0:
        break
    dyn_entries.append((tag, val))
dynstr_idx, dynstr_sh = sections[".dynstr"]
strtab = data[dynstr_sh[4]:dynstr_sh[4] + dynstr_sh[5]]
def dstr(v):
    end = strtab.index(b"\0", v)
    return strtab[v:end].decode("utf-8", "replace")

DT = {1: "NEEDED", 14: "SONAME", 0x6ffffef5: "GNU_HASH", 0x6ffffff0: "VERSYM", 0x6ffffff9: "RELACOUNT", 0x6ffffffb: "FLAGS_1"}
print("--- dynamic ---")
for tag, val in dyn_entries:
    if tag in (1, 14):
        print("  %-8s %s" % (DT.get(tag, hex(tag)), dstr(val)))

# version requirements
print("--- glibc version requirements ---")
try:
    verneed_entry = sections[".gnu.version_r"]
except KeyError:
    print("  (none)")
else:
    # .gnu.version_r 是 Elf_Verneed 单链表（vn_next 相对当前记录偏移）。
    # 防御式解析：任何越界/截断都只中止本节，绝不能挡住后面的 dynsym 导出检查。
    try:
        off = verneed_entry[1][4]
        end = off + verneed_entry[1][5]
        vers = set()
        guard = 0
        while guard < 64 and off + 16 <= end:
            vn_file, vn_aux, vn_next = struct.unpack_from("<IHI", data, off + 4)
            print("  file: %s" % dstr(vn_file))
            ao = off + vn_aux
            inner = 0
            while inner < 256 and ao + 16 <= end:
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", data, ao)
                vers.add(dstr(vna_name))
                if vna_next == 0:
                    break
                ao += vna_next
                inner += 1
            if vn_next == 0:
                break
            off += vn_next
            guard += 1
        print("  versions: %s" % ", ".join(sorted(vers)))
    except (struct.error, ValueError) as e:
        print("  (verneed parse aborted: %s)" % e)

# exported/symbols in .dynsym
print("--- dynamic symbols (defined) ---")
dynsym_idx, dynsym_sh = sections[".dynsym"]
count = dynsym_sh[5] // dynsym_sh[9]
found = []
for i in range(count):
    off = dynsym_sh[4] + i * dynsym_sh[9]
    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", data, off)
    if st_shndx == 0:
        continue
    name = dstr(st_name)
    if name:
        found.append(name)
print("  " + ", ".join(sorted(set(found))))
print("  custom_init_jsapis present: %s" % ("custom_init_jsapis" in found))
print("  internal js_post_sync leaked: %s" % ("js_post_sync" in found))
