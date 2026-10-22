"""The remastered meshes' file (.fhm, sdk/runtime/gpu/mesh_swap.h): a draw's vertices as float4 registers, its
triangle list, and (dumps) the vertex constants it was drawn with.

    python tools/meshes/fhm.py <file.fhm>...   prints what each holds
"""
import struct
import sys

import numpy as np

HEADER = struct.Struct("<4sQQIIIHHII")
CULL, CCW, CONSTANTS, SOURCE, TRI_SOURCE = 1, 2, 4, 8, 16


class Mesh:
    def __init__(self):
        self.key = self.tex = 0
        self.prim = self.count = self.flags = 0
        self.regs = 0
        self.vertices = np.zeros((0, 0, 4), np.float32)  # [vertex, register, 4]
        self.indices = np.zeros((0, 3), np.uint32)
        self.constants = None  # [192, 4]: D3D register + 96
        self.source = None  # [vertex]: the original's nearest vertex (a remastered mesh's)
        self.tri_source = None  # [triangle]: the original's triangle it was made from (a remastered mesh's)

    def reg_list(self):
        return [r for r in range(16) if self.regs >> r & 1]

    def reg(self, r):
        """the vertices' register vr, [n, 4]"""
        return self.vertices[:, self.reg_list().index(r), :]


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, key, tex, prim, count, flags, regs, nregs, nv, ni = HEADER.unpack_from(data, 0)
    if magic != b"FHM1":
        raise ValueError(f"{path}: not a mesh")
    m = Mesh()
    m.key, m.tex, m.prim, m.count, m.flags, m.regs = key, tex, prim, count, flags, regs
    o = HEADER.size
    m.vertices = np.frombuffer(data, np.float32, nv * nregs * 4, o).reshape(nv, nregs, 4).copy()
    o += nv * nregs * 16
    m.indices = np.frombuffer(data, np.uint32, ni, o).reshape(-1, 3).copy()
    o += ni * 4
    if flags & SOURCE:
        m.source = np.frombuffer(data, np.uint32, nv, o).copy()
        o += nv * 4
    if flags & TRI_SOURCE:
        m.tri_source = np.frombuffer(data, np.uint32, ni // 3, o).copy()
        o += (ni // 3) * 4
    if flags & CONSTANTS:
        m.constants = np.frombuffer(data, np.float32, 192 * 4, o).reshape(192, 4).copy()
    return m


def write(path, m):
    nv, nregs = m.vertices.shape[0], m.vertices.shape[1]
    flags = (m.flags & ~(CONSTANTS | SOURCE | TRI_SOURCE) | (SOURCE if m.source is not None else 0) |
             (TRI_SOURCE if m.tri_source is not None else 0))
    with open(path, "wb") as f:
        f.write(HEADER.pack(b"FHM1", m.key, m.tex, m.prim, m.count, flags, m.regs, nregs, nv, m.indices.size))
        f.write(np.ascontiguousarray(m.vertices, np.float32).tobytes())
        f.write(np.ascontiguousarray(m.indices, np.uint32).tobytes())
        if m.source is not None:
            f.write(np.ascontiguousarray(m.source, np.uint32).tobytes())
        if m.tri_source is not None:
            f.write(np.ascontiguousarray(m.tri_source, np.uint32).tobytes())


def main():
    for p in sys.argv[1:]:
        m = read(p)
        tris = m.indices
        degenerate = (tris[:, 0] == tris[:, 1]) | (tris[:, 1] == tris[:, 2]) | (tris[:, 0] == tris[:, 2])
        print(f"{p}: key {m.key:016x} tex {m.tex:016x} prim {m.prim} count {m.count} flags {m.flags} "
              f"regs {m.reg_list()} vertices {len(m.vertices)} triangles {len(tris)} ({degenerate.sum()} degenerate)")
        for r in m.reg_list():
            v = m.reg(r)
            print(f"  v{r}: min {np.round(v.min(0), 3)} max {np.round(v.max(0), 3)}")


if __name__ == "__main__":
    main()
