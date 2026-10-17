// The level scripts' particle calls in the log (always: a few a level; XBE_PARTICLE_LOG=0 none): SpawnParticle, SpawnParticleAtPosition,
// SpawnParticleSpawner, PlayParticleSystemAtXYZ, with their first arguments (as words, as floats, and as text when
// they point at some). Their arguments: the script stack ([0x345588]) from the call record's [0x1C] bytes less its
// [0x0C] (damage.cpp's ScriptArg).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
bool On() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_PARTICLE_LOG");
        return e && std::strcmp(e, "0") != 0;
    }();
    return on;
}

void LogCall(const char* what, X86Context& ctx) {
    if (!On()) return;
    using namespace kernel;
    uint32_t call = R32(ctx.esp + 8);
    uint32_t stack = R32(0x345588);
    uint32_t argp = R32(call + 0x1C), first = R32(call + 0xC);
    std::string line;
    for (int i = 0; i < 6; i++) {
        int32_t at = int32_t(*reinterpret_cast<const int8_t*>(HostPtr(argp + uint32_t(i)))) - int32_t(first);
        uint32_t v = R32(stack + uint32_t(at * 4));
        float f;
        std::memcpy(&f, &v, 4);
        char text[96];
        std::snprintf(text, sizeof(text), " [%d] %08X %g", i, v, f);
        line += text;
        // a pointer to text
        if (v >= 0x10000 && v < 0x4000000) {
            const char* p = reinterpret_cast<const char*>(HostPtr(v));
            bool printable = true;
            int n = 0;
            for (; n < 40 && p[n]; n++) printable &= p[n] >= 32 && p[n] < 127;
            if (printable && n >= 3) line += " \"" + std::string(p, size_t(n)) + "\"";
        }
    }
    LOGR("particles: %s%s", what, line.c_str());
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000396B0);
X86_FUNC(sub_000396B0) {
    LogCall("SpawnParticle", ctx);
    __imp__sub_000396B0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000395C0);
X86_FUNC(sub_000395C0) {
    LogCall("SpawnParticleAtPosition", ctx);
    __imp__sub_000395C0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00038CC0);
X86_FUNC(sub_00038CC0) {
    LogCall("SpawnParticleSpawner", ctx);
    __imp__sub_00038CC0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00056EF0);
X86_FUNC(sub_00056EF0) {
    LogCall("PlayParticleSystemAtXYZ", ctx);
    __imp__sub_00056EF0(ctx, base);
}

// Particle spawners (the object 0xB8480 makes): attached to an entity (0xB7EF0(entity, point, mode)), their matrix
// (at [spawner+0x4C]) found each frame by 0xB7FA0: mode 1, the point "PTnn" of the entity's model ([entity+0x264],
// 0xB51B0), else the entity's frame ([entity+0x340]); no model or no point: left as it was (the world's origin).
// Logged once per spawner (XBE_PARTICLE_LOG=0: not)
namespace {
uint32_t Tag(uint32_t t) { return t; }
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000B7EF0);
X86_FUNC(sub_000B7EF0) {
    using namespace kernel;
    uint32_t spawner = ctx.ecx, entity = R32(ctx.esp + 4), point = R32(ctx.esp + 8), mode = R32(ctx.esp + 12);
    __imp__sub_000B7EF0(ctx, base);
    if (On())
        LOGR("particles: attach spawner %08X entity %08X point %d mode %u tag %08X model %08X frame %08X", spawner, entity, int(point), mode,
             R32(spawner + 0x1C), entity ? R32(entity + 0x264) : 0, entity ? R32(entity + 0x340) : 0);
}

X86_EXTERN_FUNC(__imp__sub_000B7FA0);
X86_FUNC(sub_000B7FA0) {
    using namespace kernel;
    uint32_t spawner = ctx.ecx;
    __imp__sub_000B7FA0(ctx, base);
    if (!On()) return;
    static std::unordered_map<uint32_t, int> told;
    int& n = told[spawner];
    if (n++ % 600 != 0 || told.size() > 400) return;
    uint32_t mode = R32(spawner + 0x20), m = R32(spawner + 0x4C);
    uint32_t entity = mode == 2 ? R32(spawner + 0x28) : 0;
    float pos[3] = {};
    if (m) for (int i = 0; i < 3; i++) { uint32_t v = R32(m + 0x30 + uint32_t(i) * 4); std::memcpy(&pos[i], &v, 4); }
    LOGR("particles: spawner %08X mode %u tag %08X matrix %08X at %.1f %.1f %.1f (entity link %08X)", spawner, mode, R32(spawner + 0x1C), m,
         pos[0], pos[1], pos[2], entity);
}

// A particle instance drawn (0xBA580, this: the instance; its matrix the first 64 bytes, its system at +0x44, flags at
// +0x40): instances placed at the world's origin logged with their contents, a few placed ones too, once each
X86_EXTERN_FUNC(__imp__sub_000BA580);
X86_FUNC(sub_000BA580) {
    using namespace kernel;
    uint32_t inst = ctx.ecx;
    if (On() && inst) {
        static std::unordered_map<uint32_t, int> seen;
        static int placed = 0;
        float pos[3];
        for (int i = 0; i < 3; i++) { uint32_t v = R32(inst + 0x30 + uint32_t(i) * 4); std::memcpy(&pos[i], &v, 4); }
        bool origin = std::fabs(pos[0]) < 1 && std::fabs(pos[1]) < 1 && std::fabs(pos[2]) < 1;
        if ((origin || placed < 6) && seen.size() < 200 && seen.emplace(inst, 0).second) {
            if (!origin) placed++;
            std::string dump;
            for (uint32_t o = 0; o < 0x80; o += 4) {
                char t[12];
                std::snprintf(t, sizeof(t), " %08X", R32(inst + 0x40 + o));
                dump += t;
            }
            LOGR("particles: instance %08X%s vt %08X at %.1f %.1f %.1f flags %08X system %08X tex %08X ret %08X %08X | +40:%s", inst,
                 origin ? " AT ORIGIN" : "", R32(inst), pos[0], pos[1], pos[2], R32(inst + 0x40), R32(inst + 0x44), R32(inst + 0x5C),
                 R32(ctx.esp), R32(ctx.esp + 0x20), dump.c_str());
        }
    }
    __imp__sub_000BA580(ctx, base);
}

// A particle instance placed (0xBA200, this: the instance; the system, the placement matrix): logged with its callers
X86_EXTERN_FUNC(__imp__sub_000BA200);
X86_FUNC(sub_000BA200) {
    using namespace kernel;
    uint32_t inst = ctx.ecx, system = R32(ctx.esp + 4), m = R32(ctx.esp + 8);
    if (On()) {
        static std::unordered_map<uint32_t, int> per_system;
        if (per_system[system]++ < 3) {
            float pos[3] = {};
            if (m) for (int i = 0; i < 3; i++) { uint32_t v = R32(m + 0x30 + uint32_t(i) * 4); std::memcpy(&pos[i], &v, 4); }
            uint32_t code[6] = {}, n = 0;
            for (uint32_t i = 0; i < 96 && n < 6; i++) {
                uint32_t v = R32(ctx.esp + i * 4);
                if (v >= 0x11000 && v < 0x2C0000) code[n++] = v;
            }
            LOGR("particles: place %08X system %08X matrix %08X at %.1f %.1f %.1f from %08X %08X %08X %08X %08X %08X", inst, system, m, pos[0],
                 pos[1], pos[2], code[0], code[1], code[2], code[3], code[4], code[5]);
        }
    }
    __imp__sub_000BA200(ctx, base);
}

// A particle instance placed by the system's kind (0xBA6B0: 0xBA200, or its own placement for the simple kinds;
// this: the instance, then the system and the placement matrix): logged per system, with the callers
X86_EXTERN_FUNC(__imp__sub_000BA6B0);
X86_FUNC(sub_000BA6B0) {
    using namespace kernel;
    uint32_t inst = ctx.ecx, system = R32(ctx.esp + 4), m = R32(ctx.esp + 8);
    if (On()) {
        static std::unordered_map<uint32_t, int> per_system;
        if (per_system[system]++ < 4) {
            float pos[3] = {};
            if (m && m < 0x4000000 - 0x40) for (int i = 0; i < 3; i++) { uint32_t v = R32(m + 0x30 + uint32_t(i) * 4); std::memcpy(&pos[i], &v, 4); }
            uint32_t code[8] = {}, n = 0;
            for (uint32_t i = 0; i < 160 && n < 8; i++) {
                uint32_t v = R32(ctx.esp + i * 4);
                if (v >= 0x11000 && v < 0x2C0000) code[n++] = v;
            }
            LOGR("particles: create %08X system %08X kind %u matrix %08X at %.1f %.1f %.1f from %08X %08X %08X %08X %08X %08X %08X %08X", inst,
                 system, system ? R32(system) : 0, m, pos[0], pos[1], pos[2], code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
        }
    }
    __imp__sub_000BA6B0(ctx, base);
}
