#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include "gpu/state.h"
#include "gpu/vertex.h"

// remastered meshes drawn in place of the game's own: a draw's vertices as floats (AssembleFloats) keyed by their
// hash, replaced by a mesh with the same input registers (the bind pose's positions, weights, bone indices, normals,
// texture coordinates), so the game's own vertex program skins and animates it as it did the original.
// Files: <remaster>/meshes/<key>.fhm, written by the dump (XBE_MESH_DUMP=<folder>) and the tools/meshes pipeline
namespace gpu::meshes {

// the file's flags
enum : uint32_t {
    kMeshCull = 1,       // the draw culled back faces
    kMeshCcw = 2,        // culling clockwise: counter clockwise is the front
    kMeshConstants = 4,  // the vertex constants follow (192 float4, the dump's)
    kMeshSource = 8,     // each vertex's nearest vertex of the original follows the indices (uint32)
    kMeshTriSource = 16, // each triangle's own of the original (its index in the draw's order) follows (uint32)
};

struct Mesh {
    uint16_t regs = 0;
    uint32_t vertex_count = 0;
    std::vector<float> vertices;    // vertex_count * registers * 4
    std::vector<uint32_t> indices;  // a triangle list
    std::vector<uint32_t> source;   // each vertex's nearest of the original's (its AssembleFloats order), or empty
    std::vector<uint32_t> tri_source;  // each triangle's own of the original's (the draw's triangle order), or empty
};

// loads <dir>/*.fhm once (the remaster folder's meshes)
void Load(const std::filesystem::path& dir);

// whether any draw of this primitive and vertex count may be replaced (cheap: before anything is hashed)
bool Candidate(uint32_t prim, uint32_t count);

// the draw's floats (AssembleFloats) hashed
uint64_t Key(const Assembled& a);

// a draw by its buffers (where its vertices and indices are, how it reads them) and a check of their first bytes:
// the key found for it kept, worked out again only when those bytes change (vertices written anew)
struct DrawId {
    uint64_t where = 0, check = 0;
};
DrawId IdOf(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
bool CachedKey(const DrawId& id, uint64_t& key);
void StoreKey(const DrawId& id, uint64_t key);

// the mesh drawn for the draw keyed so with these registers, null if none
const Mesh* Find(uint64_t key, uint16_t regs);

// a drawn with the mesh: its vertices and indices (kept on the GPU, Assembled::resident)
void Fill(const Mesh& m, Assembled& a);

// the dump folder (XBE_MESH_DUMP), empty when off
const std::filesystem::path& DumpDir();

// writes the draw's floats to <DumpDir>/<key>.fhm, once a key, when the same key has been seen in three frames
// (vertices made anew every frame, a cloth's or a morph's, are left)
void Dump(const State& s, uint32_t prim, uint32_t count, uint64_t key, uint64_t tex, const Assembled& a, uint64_t frame);

}  // namespace gpu::meshes
