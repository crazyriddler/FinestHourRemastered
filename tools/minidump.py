"""A Windows minidump's crash (no debugger needed): the exception, where it happened (module + offset), the faulting
thread's registers, and the return addresses on its stack that fall in a module (a raw scan, not an unwind).

    python tools/minidump.py <file.dmp> [--stack 400]
"""
import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--stack", type=int, default=400, help="stack slots scanned for return addresses")
    a = ap.parse_args()
    d = open(a.dump, "rb").read()
    if d[:4] != b"MDMP":
        sys.exit("not a minidump")
    n, rva = struct.unpack_from("<II", d, 8)
    streams = {}
    for i in range(n):
        t, size, at = struct.unpack_from("<III", d, rva + i * 12)
        streams.setdefault(t, (size, at))

    def name_at(r):
        ln = struct.unpack_from("<I", d, r)[0]
        return d[r + 4:r + 4 + ln].decode("utf-16-le")

    modules = []
    if 4 in streams:
        _, at = streams[4]
        cnt = struct.unpack_from("<I", d, at)[0]
        for i in range(cnt):
            m = at + 4 + i * 108
            base, size, _, ts, nrva = struct.unpack_from("<QIIII", d, m)
            modules.append((base, size, name_at(nrva).split("\\")[-1], ts))

    def where(addr):
        for base, size, nm, _ in modules:
            if base <= addr < base + size:
                return f"{nm}+0x{addr - base:X}"
        return f"0x{addr:X}"

    # memory: (start, size, file offset)
    mem = []
    if 9 in streams:
        _, at = streams[9]
        cnt, base_rva = struct.unpack_from("<QQ", d, at)
        off = base_rva
        for i in range(cnt):
            start, size = struct.unpack_from("<QQ", d, at + 16 + i * 16)
            mem.append((start, size, off))
            off += size
    if 5 in streams:
        _, at = streams[5]
        cnt = struct.unpack_from("<I", d, at)[0]
        for i in range(cnt):
            start, size, r = struct.unpack_from("<QII", d, at + 4 + i * 16)
            mem.append((start, size, r))

    def read(addr, ln):
        for start, size, off in mem:
            if start <= addr and addr + ln <= start + size:
                return d[off + addr - start:off + addr - start + ln]
        return None

    exe = modules[0] if modules else None
    if exe:
        print(f"exe {exe[2]} base 0x{exe[0]:X} size 0x{exe[1]:X} timestamp 0x{exe[3]:X}")
    if 6 not in streams:
        sys.exit("no exception stream")
    _, at = streams[6]
    tid = struct.unpack_from("<I", d, at)[0]
    code, flags, _, addr, nparams = struct.unpack_from("<IIQQI", d, at + 8)
    params = struct.unpack_from("<15Q", d, at + 8 + 32)[:nparams]
    ctx_size, ctx_rva = struct.unpack_from("<II", d, at + 8 + 32 + 120)
    print(f"thread {tid}: exception 0x{code:08X} at {where(addr)} params {[hex(p) for p in params]}")
    c = d[ctx_rva:ctx_rva + ctx_size]
    names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
    regs = dict(zip(names, struct.unpack_from("<16Q", c, 0x78)))
    regs["rip"] = struct.unpack_from("<Q", c, 0xF8)[0]
    print("  " + " ".join(f"{k} {v:X}" for k, v in regs.items()))
    rsp = regs["rsp"]
    print("stack (return addresses in modules):")
    for i in range(a.stack):
        b = read(rsp + i * 8, 8)
        if b is None:
            break
        v = struct.unpack("<Q", b)[0]
        w = where(v)
        if "+0x" in w:
            print(f"  [rsp+{i * 8:X}] {w}")


if __name__ == "__main__":
    main()
