// The renderer's graphics API (gpu/rhi.h) done by Vulkan 1.3, natively:
//   - dynamic rendering (no render pass objects), its instances begun when a draw needs its targets and ended by
//     anything that may not run inside one (barriers, copies)
//   - synchronization2 barriers from each resource's state as the recorded commands left it (its layout, the stages
//     and accesses of its last use), several gathered into one call
//   - the shaders' textures and samplers as push descriptors (set 0), pushed when a draw's tables or pipeline changed,
//     only the bindings the pipeline's shaders use; their constant buffers as dynamic uniform buffers (set 1) of the
//     upload buffer they lie in, a draw moving their offsets alone
//   - memory by the Vulkan Memory Allocator; HLSL compiled to SPIR-V by DXC (dxcompiler.dll beside the exe), the
//     stages' inputs matched to the outputs before them by semantic, as Direct3D matches them
//   - the remastered textures copied on a transfer queue of their own when the device has one
#include "gpu/rhi.h"
#include "gpu/keymap.h"

#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <vk_mem_alloc.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace gpu::rhi {

bool UseVulkan() {
    // tests: XBE_RENDERER=vulkan or d3d12 over the setting
    if (const char* e = std::getenv("XBE_RENDERER")) return std::strcmp(e, "vulkan") == 0;
    return app::Get().renderer == 1;
}

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kTBinding = 8;    // t0..t9 at bindings 8..17 of set 0
constexpr uint32_t kSBinding = 24;   // s0..s6 at 24..30 (s4, s5, s6 fixed)
constexpr uint32_t kTextures = 10, kSamplers = 4, kCbvs = 5;
constexpr VkDeviceSize kCbvRange = 65536;  // a constant buffer's most (D3D12's)

// ----------------------------------------------------------------------------------------------------------------
// formats

struct Fmt {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    uint32_t bytes = 0;  // a texel's, a block's
    uint32_t block = 1;  // texels a block side
    bool depth = false, stencil = false;
};

VkFormat g_depth_format = VK_FORMAT_D24_UNORM_S8_UINT;  // D24S8 where the device has it, else D32S8

Fmt FormatOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        case DXGI_FORMAT_R32G32B32A32_UINT: return {VK_FORMAT_R32G32B32A32_UINT, 16};
        case DXGI_FORMAT_R32G32B32_FLOAT: return {VK_FORMAT_R32G32B32_SFLOAT, 12};
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
        case DXGI_FORMAT_R16G16B16A16_UNORM: return {VK_FORMAT_R16G16B16A16_UNORM, 8};
        case DXGI_FORMAT_R16G16B16A16_SNORM: return {VK_FORMAT_R16G16B16A16_SNORM, 8};
        case DXGI_FORMAT_R16G16B16A16_SINT: return {VK_FORMAT_R16G16B16A16_SINT, 8};
        case DXGI_FORMAT_R16G16B16A16_UINT: return {VK_FORMAT_R16G16B16A16_UINT, 8};
        case DXGI_FORMAT_R32G32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, 8};
        case DXGI_FORMAT_R10G10B10A2_UNORM: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4};
        case DXGI_FORMAT_R11G11B10_FLOAT: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4};
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 4};
        case DXGI_FORMAT_R8G8B8A8_UINT: return {VK_FORMAT_R8G8B8A8_UINT, 4};
        case DXGI_FORMAT_R8G8B8A8_SNORM: return {VK_FORMAT_R8G8B8A8_SNORM, 4};
        case DXGI_FORMAT_R8G8B8A8_SINT: return {VK_FORMAT_R8G8B8A8_SINT, 4};
        case DXGI_FORMAT_R16G16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, 4};
        case DXGI_FORMAT_R16G16_UNORM: return {VK_FORMAT_R16G16_UNORM, 4};
        case DXGI_FORMAT_R16G16_SNORM: return {VK_FORMAT_R16G16_SNORM, 4};
        case DXGI_FORMAT_R16G16_SINT: return {VK_FORMAT_R16G16_SINT, 4};
        case DXGI_FORMAT_R16G16_UINT: return {VK_FORMAT_R16G16_UINT, 4};
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT: return {VK_FORMAT_R32_SFLOAT, 4};
        case DXGI_FORMAT_D32_FLOAT: return {VK_FORMAT_D32_SFLOAT, 4, 1, true};
        case DXGI_FORMAT_R32_UINT: return {VK_FORMAT_R32_UINT, 4};
        case DXGI_FORMAT_R32_SINT: return {VK_FORMAT_R32_SINT, 4};
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return {g_depth_format, 4, 1, true, true};
        case DXGI_FORMAT_R8G8_UNORM: return {VK_FORMAT_R8G8_UNORM, 2};
        case DXGI_FORMAT_R8G8_UINT: return {VK_FORMAT_R8G8_UINT, 2};
        case DXGI_FORMAT_R8G8_SNORM: return {VK_FORMAT_R8G8_SNORM, 2};
        case DXGI_FORMAT_R16_FLOAT: return {VK_FORMAT_R16_SFLOAT, 2};
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM: return {VK_FORMAT_R16_UNORM, 2};
        case DXGI_FORMAT_D16_UNORM: return {VK_FORMAT_D16_UNORM, 2, 1, true};
        case DXGI_FORMAT_R16_UINT: return {VK_FORMAT_R16_UINT, 2};
        case DXGI_FORMAT_R16_SNORM: return {VK_FORMAT_R16_SNORM, 2};
        case DXGI_FORMAT_R16_SINT: return {VK_FORMAT_R16_SINT, 2};
        case DXGI_FORMAT_R8_UNORM: return {VK_FORMAT_R8_UNORM, 1};
        case DXGI_FORMAT_R8_UINT: return {VK_FORMAT_R8_UINT, 1};
        case DXGI_FORMAT_A8_UNORM: return {VK_FORMAT_R8_UNORM, 1};
        case DXGI_FORMAT_BC1_UNORM: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4};
        case DXGI_FORMAT_BC2_UNORM: return {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4};
        case DXGI_FORMAT_BC3_UNORM: return {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4};
        case DXGI_FORMAT_BC4_UNORM: return {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4};
        case DXGI_FORMAT_BC5_UNORM: return {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4};
        case DXGI_FORMAT_BC7_UNORM: return {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4};
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, 4};
        default: return {};
    }
}

bool IsTypeless(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_R16_TYPELESS ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

VkCompareOp CompareOp(D3D12_COMPARISON_FUNC f) {
    switch (f) {
        case D3D12_COMPARISON_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
        case D3D12_COMPARISON_FUNC_LESS: return VK_COMPARE_OP_LESS;
        case D3D12_COMPARISON_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
        case D3D12_COMPARISON_FUNC_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case D3D12_COMPARISON_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
        case D3D12_COMPARISON_FUNC_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp StencilOp(D3D12_STENCIL_OP o) {
    switch (o) {
        case D3D12_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
        case D3D12_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
        case D3D12_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case D3D12_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case D3D12_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
        case D3D12_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case D3D12_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor BlendFactor(D3D12_BLEND b) {
    switch (b) {
        case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
        case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
        case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(D3D12_BLEND_OP o) {
    switch (o) {
        case D3D12_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case D3D12_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case D3D12_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
        case D3D12_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkSamplerAddressMode AddressMode(D3D12_TEXTURE_ADDRESS_MODE m) {
    switch (m) {
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkPrimitiveTopology Topology(D3D_PRIMITIVE_TOPOLOGY t) {
    switch (t) {
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// ----------------------------------------------------------------------------------------------------------------
// a use of a resource: where its last access ran, what it was, the image's layout then

struct Use {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                   VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

const Use kRender{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
const Use kDepthWrite{kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
const Use kSampled{kShaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
const Use kCopySrc{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
const Use kCopyDst{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};

Use UseOf(D3D12_RESOURCE_STATES s, bool swapchain) {
    if (s == D3D12_RESOURCE_STATE_COMMON) {  // (also PRESENT)
        // (all commands: the next barrier from it chains to this one's wait, a copy of the shown frame after it)
        if (swapchain) return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    if (s & D3D12_RESOURCE_STATE_RENDER_TARGET) return kRender;
    if (s & D3D12_RESOURCE_STATE_DEPTH_WRITE) return kDepthWrite;
    if (s & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST)) return kCopyDst;
    Use u;
    u.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (s & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        u.stage |= kShaders, u.access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (s & D3D12_RESOURCE_STATE_DEPTH_READ)
        u.stage |= kDepthStages, u.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            u.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (s & (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE)) {
        u.stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, u.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        u.layout = u.access == VK_ACCESS_2_TRANSFER_READ_BIT ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    }
    if (s & (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER))
        u.stage |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | kShaders,
            u.access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
    if (s == D3D12_RESOURCE_STATE_GENERIC_READ)
        u = {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    if (!u.stage) u = {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    return u;
}

bool Readable(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_GENERAL;
}

// ----------------------------------------------------------------------------------------------------------------
// SPIR-V: the stages' interfaces (by DXC's names, in.var.<SEMANTIC> and out.var.<SEMANTIC>) and resources

struct Spirv {
    struct Var {
        uint32_t storage = ~0u;
        std::string name;
        size_t location = 0;  // the word holding its Location, 0 none
        bool patch = false, builtin = false;
        uint32_t set = ~0u, binding = ~0u;
    };
    std::vector<uint32_t> w;
    KeyMap<uint32_t, Var> vars;
    std::string entry = "main";

    explicit Spirv(const Blob* b) {
        w.resize(b->GetBufferSize() / 4);
        std::memcpy(w.data(), b->GetBufferPointer(), w.size() * 4);
        Parse();
    }
    void Parse() {
        for (size_t i = 5; i < w.size();) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
            if (!n || i + n > w.size()) break;
            if (op == 15 && n >= 4) {  // OpEntryPoint
                entry = reinterpret_cast<const char*>(&w[i + 3]);
            } else if (op == 5 && n >= 3) {  // OpName
                vars[w[i + 1]].name = reinterpret_cast<const char*>(&w[i + 2]);
            } else if (op == 71 && n >= 3) {  // OpDecorate
                Var& v = vars[w[i + 1]];
                uint32_t dec = w[i + 2];
                if (dec == 30 && n >= 4) v.location = i + 3;
                else if (dec == 11) v.builtin = true;
                else if (dec == 15) v.patch = true;
                else if (dec == 34 && n >= 4) v.set = w[i + 3];
                else if (dec == 33 && n >= 4) v.binding = w[i + 3];
            } else if (op == 59 && n >= 4) {  // OpVariable
                vars[w[i + 2]].storage = w[i + 3];
            }
            i += n;
        }
    }
    // a stage variable's semantic: upper case, an index always (COLOR -> COLOR0)
    static std::string Semantic(const std::string& name) {
        std::string s;
        if (name.rfind("in.var.", 0) == 0) s = name.substr(7);
        else if (name.rfind("out.var.", 0) == 0) s = name.substr(8);
        else return "";
        for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.back()))) s += '0';
        return s;
    }
    // storage class 1 input, 3 output: semantic (patch ones apart) to location
    std::unordered_map<std::string, uint32_t> Interface(uint32_t storage) const {
        std::unordered_map<std::string, uint32_t> m;
        for (auto& [id, v] : vars)
            if (v.storage == storage && v.location && !v.builtin) m[(v.patch ? "P:" : "") + Semantic(v.name)] = w[v.location];
        return m;
    }
    // the inputs placed where the stage before puts its outputs of the same semantics; false when one has none
    bool MatchInputs(const std::unordered_map<std::string, uint32_t>& outputs, std::string& missing) {
        for (auto& [id, v] : vars) {
            if (v.storage != 1 || !v.location || v.builtin) continue;
            auto it = outputs.find((v.patch ? "P:" : "") + Semantic(v.name));
            if (it == outputs.end()) {
                missing = v.name;
                return false;
            }
            w[v.location] = it->second;
        }
        return true;
    }
    // false for a resource the layout has no place for (a structured buffer)
    bool Bindings(uint32_t& textures, uint32_t& samplers, uint32_t& cbvs) const {
        for (auto& [id, v] : vars) {
            if (v.binding == ~0u || v.set == ~0u) continue;
            if (v.storage == 12) return false;  // StorageBuffer
            if (v.set == 1 && v.binding < kCbvs) cbvs |= 1u << v.binding;
            else if (v.set == 0 && v.binding >= kTBinding && v.binding < kTBinding + kTextures) textures |= 1u << (v.binding - kTBinding);
            else if (v.set == 0 && v.binding >= kSBinding && v.binding < kSBinding + kSamplers) samplers |= 1u << (v.binding - kSBinding);
        }
        return true;
    }
};

// ----------------------------------------------------------------------------------------------------------------
// DXC

using DxcCreate = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

DxcCreate DxcLoader() {
    static DxcCreate create = [] {
        HMODULE m = LoadLibraryW((app::ExeDir() / "dxcompiler.dll").c_str());
        if (!m) m = LoadLibraryW(L"dxcompiler.dll");
        if (!m) LOGE("vulkan: dxcompiler.dll not found beside the exe: no shaders");
        return m ? reinterpret_cast<DxcCreate>(GetProcAddress(m, "DxcCreateInstance")) : nullptr;
    }();
    return create;
}

std::vector<std::wstring> DxcArgs() {
    std::vector<std::wstring> a = {L"-spirv", L"-fspv-target-env=vulkan1.3", L"-fvk-use-dx-layout", L"-fvk-use-dx-position-w",
                                   L"-HV", L"2018", L"-O3"};
    auto bind = [&](wchar_t type, uint32_t reg, uint32_t binding, uint32_t set) {
        a.push_back(L"-fvk-bind-register");
        a.push_back(std::wstring(1, type) + std::to_wstring(reg));
        a.push_back(L"0");
        a.push_back(std::to_wstring(binding));
        a.push_back(std::to_wstring(set));
    };
    for (uint32_t i = 0; i < kTextures; i++) bind(L't', i, kTBinding + i, 0);
    for (uint32_t i = 0; i <= kSamplers + 2; i++) bind(L's', i, kSBinding + i, 0);
    for (uint32_t i = 0; i < kCbvs; i++) bind(L'b', i, i, 1);
    // tests: XBE_VK_DXC="arg arg": more arguments (a -fspv-target-env given there in place of the one above)
    if (const char* e = std::getenv("XBE_VK_DXC")) {
        std::string s = e, w;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i < s.size() && s[i] != ' ') {
                w += s[i];
                continue;
            }
            if (w.empty()) continue;
            std::wstring ww(w.begin(), w.end());
            if (ww.rfind(L"-fspv-target-env=", 0) == 0) a[1] = ww;
            else a.push_back(ww);
            w.clear();
        }
    }
    return a;
}

// ----------------------------------------------------------------------------------------------------------------
// objects

class VkDev;

class VkRes final : public Resource {
public:
    VkDev& d;
    VkBuffer buf = VK_NULL_HANDLE;
    VkImage img = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    uint32_t id = 0;  // a buffer's number (its GpuVa's top bits)
    uint64_t serial = 0;  // made the serial'th: a view's place names it so, released or not (VkDev::live)
    uint32_t place = 0;   // its place in VkDev::live while it is there
    bool swapchain = false;
    bool readback = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    Fmt fmt;
    VkImageAspectFlags aspect = 0;
    uint32_t mips = 1, layers = 1, samples = 1;
    VkExtent3D extent{1, 1, 1};
    bool is3d = false, cube = false, mutable_format = false;
    // a target new: zeros before its first use, as Direct3D 12 makes them (its memory may hold anything: a float
    // target's NaNs, read by a pass before it is drawn, spread through the frame)
    bool zero = false;
    // a whole clear not done yet: the next rendering instance that has it loads it cleared, anything else first clears it
    bool pending = false;
    VkImageAspectFlags pending_aspect = 0;
    VkClearValue pending_value{};
    VkImageView pending_view = VK_NULL_HANDLE;
    VkExtent2D pending_extent{};
    Use use;  // as the recorded commands leave it (the main queue's)
    struct ViewKey {
        VkFormat format;
        VkImageViewType type;
        VkImageAspectFlags aspect;
        uint32_t mip, mips, layer, layers;
        VkComponentMapping swizzle;
        bool operator==(const ViewKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    std::vector<std::pair<ViewKey, VkImageView>> views;
    VkDescriptorSet ubo = VK_NULL_HANDLE;  // an upload buffer's constant buffers (set 1)
    // filled by the uploader: the main queue takes it with a barrier (another queue family) or as it is
    bool acquire = false;
    explicit VkRes(VkDev& dev) : d(dev) {}
    void Describe(const D3D12_RESOURCE_DESC& desc, HeapType heap) { desc_ = desc, heap_ = heap; }
    ~VkRes() override;
    GpuVa Va() const override { return GpuVa(id) << 40; }
    uint8_t* Map() override;
    VkImageView View(const ViewKey& k);
};

class VkPipe final : public Pipeline {
public:
    VkDev& d;
    VkPipeline p = VK_NULL_HANDLE;
    uint32_t textures = 0, samplers = 0, cbvs = 0;  // the bindings its shaders use
    std::vector<std::vector<uint32_t>> code;           // (XBE_VK_CHECKPOINTS: its stages' SPIR-V, for a fault's report)
    bool gs = false, tess = false;                     // (tests: XBE_VK_SKIP)
    uint32_t number = 0;                               // (XBE_VK_TRACE: its number, logged at its first draw)
    bool drawn = false;
    explicit VkPipe(VkDev& dev) : d(dev) {}
    ~VkPipe() override;
};

class VkQueries final : public QueryPool {
public:
    VkDev& d;
    VkQueryPool pool = VK_NULL_HANDLE;
    explicit VkQueries(VkDev& dev) : d(dev) {}
    ~VkQueries() override;
};

// a place in a table of views
struct Slot {
    VkRes* res = nullptr;  // (not counted: as D3D12's descriptors; serial tells whether it is still there)
    uint64_t serial = 0;
    uint32_t place = 0;    // res's place in VkDev::live
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint8_t null_kind = 0;  // a null shader resource view: its dummy (by view type)
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageAspectFlags aspect = 0;
};

enum NullKind : uint8_t { kNull2D, kNull2DArray, kNullCube, kNull3D, kNull2DMS, kNullKinds };

class VkList final : public List {
public:
    VkDev& d;
    bool main;  // the main queue's (else the uploader's)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    explicit VkList(VkDev& dev, bool is_main) : d(dev), main(is_main) {}
    void Begin(VkCommandBuffer c);
    void End();

    void SetLayout(Layout l) override { layout_ = l; }
    void SetPipeline(Pipeline* p) override {
        auto* vp = static_cast<VkPipe*>(p);
        if (vp == pipe_) return;
        if (!pipe_ || !vp || pipe_->cbvs != vp->cbvs) cbv_dirty_ = true;
        pipe_ = vp;
        pipe_dirty_ = true;
    }
    void SetCbv(uint32_t param, GpuVa va) override;
    void SetTable(uint32_t param, Table t) override;
    // (each set only when it differs from what this command buffer has: dyn_set_, a bit a state)
    void SetViewport(const D3D12_VIEWPORT& v) override {
        float w = std::max(v.Width, 1.0f), h = std::max(v.Height, 1.0f);
        VkViewport vp{v.TopLeftX, v.TopLeftY + h, w, -h, v.MinDepth, v.MaxDepth};
        if ((dyn_set_ & 1) && !std::memcmp(&vp, &vp_, sizeof(vp))) return;
        vkCmdSetViewport(cb, 0, 1, &vp);
        vp_ = vp, dyn_ok_ = true, dyn_set_ |= 1;
    }
    void SetScissor(const D3D12_RECT& r) override {
        LONG x = std::max<LONG>(r.left, 0), y = std::max<LONG>(r.top, 0);
        VkRect2D s{{x, y}, {uint32_t(std::max<LONG>(r.right - x, 0)), uint32_t(std::max<LONG>(r.bottom - y, 0))}};
        if ((dyn_set_ & 2) && !std::memcmp(&s, &sc_, sizeof(s))) return;
        vkCmdSetScissor(cb, 0, 1, &s);
        sc_ = s, dyn_set_ |= 2;
    }
    void SetBlendFactor(const float f[4]) override {
        if ((dyn_set_ & 4) && !std::memcmp(f, bf_, 16)) return;
        vkCmdSetBlendConstants(cb, f), std::memcpy(bf_, f, 16), dyn_set_ |= 4;
    }
    void SetStencilRef(uint32_t r) override {
        if ((dyn_set_ & 8) && r == sref_) return;
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, r), sref_ = r, dyn_set_ |= 8;
    }
    void SetTopology(D3D_PRIMITIVE_TOPOLOGY t) override {
        const VkPrimitiveTopology vt = Topology(t);
        if ((dyn_set_ & 16) && vt == topo_) return;
        vkCmdSetPrimitiveTopology(cb, topo_ = vt), dyn_set_ |= 16;
    }
    void SetVertexBuffers(uint32_t start, uint32_t n, const D3D12_VERTEX_BUFFER_VIEW* v) override;
    void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) override;
    void SetTargets(uint32_t n, const View* rtv, const View* dsv) override;
    void ClearRtv(View v, const float color[4], uint32_t n, const D3D12_RECT* rects) override;
    void ClearDsv(View v, D3D12_CLEAR_FLAGS flags, float z, uint8_t stencil, uint32_t n, const D3D12_RECT* rects) override;
    uint64_t draws = 0, dropped = 0, passes = 0, barriers = 0, clears = 0;  // (diagnostics)
    void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) override {
        draws++;
        if (!Prepare()) return dropped++, void();
        vkCmdDraw(cb, count, instances, first, first_instance);
        pass_draws_++;
        Checkpoint("draw");
        Split(count);
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance) override {
        draws++;
        if (!Prepare()) return dropped++, void();
        vkCmdDrawIndexed(cb, count, instances, first, base, first_instance);
        pass_draws_++;
        Checkpoint("draw indexed");
        Split(count);
    }
    void Barrier(Resource* r, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES after) override {
        auto* x = static_cast<VkRes*>(r);
        Transition(x, UseOf(after, x->swapchain));
    }
    void CopyTexture(const CopyLocation& dst, uint32_t x, uint32_t y, uint32_t z, const CopyLocation& src, const D3D12_BOX* box) override;
    void CopyBuffer(Resource* dst, uint64_t dst_offset, Resource* src, uint64_t src_offset, uint64_t bytes) override;
    void CopyResource(Resource* dst, Resource* src) override;
    void Resolve(Resource* dst, Resource* src, DXGI_FORMAT fmt) override;
    void Timestamp(QueryPool* q, uint32_t i) override;
    void ResolveTimestamps(QueryPool* q, uint32_t first, uint32_t n, Resource* dst, uint64_t offset) override;
    void Acquire(Resource* r) override;
    void Checkpoint(const char* what);
    void Split(uint32_t count);

    // the resource's next use: a barrier from its last one when the two need it (gathered until the next command)
    void Transition(VkRes* r, const Use& to, bool keep_clear = false);
    void ZeroFill(VkRes* r);
    void FlushClear(VkRes* r);
    void FlushClears();
    void Defer(VkRes* r, VkImageView view, VkExtent2D extent, VkImageAspectFlags aspect, const VkClearValue& v);
    std::vector<VkRes*> pending_clears;
    void FlushBarriers();
    void EndRendering(const char* why = "?");
    bool dump = false;  // (XBE_VK_DUMP_FRAME: the frame's passes logged)
    uint32_t pass_draws_ = 0;
    std::vector<VkRes*> uploaded;  // the uploader's: textures it copied into this time

private:
    bool Prepare();
    void BeginRendering();
    bool Attachments(VkRenderingAttachmentInfo* colors, uint32_t& nc, VkRenderingAttachmentInfo& depth, bool& has_depth,
                     VkExtent2D& area);
    void PushDescriptors();
    void BindCbvs();
    VkImageView SrvView(uint32_t slot, uint8_t kind_if_null, VkImageLayout& layout);

    Layout layout_ = Layout::Main;
    // the dynamic states as set (XBE_VK_SPLIT's command buffers set them again)
    VkViewport vp_{};
    VkRect2D sc_{};
    float bf_[4] = {};
    uint32_t sref_ = 0;
    VkPrimitiveTopology topo_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool dyn_ok_ = false;
    uint8_t dyn_set_ = 0;  // the dynamic states this command buffer has as vp_, sc_, bf_, sref_, topo_ (a bit each)
    VkPipe* pipe_ = nullptr;
    VkPipe* bound_pipe_ = nullptr;
    bool pipe_dirty_ = true;
    // the tables (slot indices), by register: t0..t6 (main 2, motion 5), t7..t9 (main 4), s0..s3 (main 3)
    uint32_t t_base_[2] = {~0u, ~0u};
    uint32_t s_base_ = ~0u;
    bool desc_dirty_ = true;
    uint32_t pushed_t_ = 0, pushed_s_ = 0;           // the bindings pushed since the tables last moved
    VkImageLayout pushed_layout_[16] = {};           // each texture binding's layout as pushed
    GpuVa cbv_[kCbvs] = {};
    bool cbv_dirty_ = true;
    // the vertex and index buffers
    D3D12_VERTEX_BUFFER_VIEW vb_[16] = {};
    uint32_t vb_dirty_ = 0;
    D3D12_INDEX_BUFFER_VIEW ib_{};
    bool ib_dirty_ = false;
    // the targets: as set, and the rendering instance begun
    View rtv_[8]{};
    uint32_t rtv_n_ = 0;
    View dsv_{};
    bool has_dsv_ = false;
    bool rendering_ = false;
    std::vector<VkImageMemoryBarrier2> image_barriers_;
    VkMemoryBarrier2 mem_barrier_{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkRenderingAttachmentInfo stencil_{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
};

// D3D12's pipeline library's part: pipelines made through the device's VkPipelineCache
class VkPipeCache final : public PipelineCache {
public:
    VkDev& d;
    explicit VkPipeCache(VkDev& dev) : d(dev) {}
    bool Load(const wchar_t*, const PipelineDesc& pd, Ref<Pipeline>& out) override;
    bool Store(const wchar_t*, Pipeline*) override { return true; }
    std::vector<uint8_t> Serialize() override;
};

class VkUploader final : public Uploader {
public:
    VkDev& d;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::mutex* queue_lock = nullptr;  // shared with the main queue's when it is the same queue
    VkCommandPool pool[2] = {};
    VkCommandBuffer cbs[2] = {};
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t value = 0;
    uint32_t slot = 0;
    VkList list;
    explicit VkUploader(VkDev& dev) : d(dev), list(dev, false) {}
    ~VkUploader() override;
    List* Begin(uint32_t s) override;
    uint64_t Submit() override;
    uint64_t Completed() override;
    void Wait(uint64_t v) override;
};

class VkDev final : public Device {
public:
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0, copy_family = ~0u, copy_index = 0;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queue_lock;
    VmaAllocator vma = nullptr;
    bool custom_border = false;
    bool device_fault = false;
    // presentation
    HWND hwnd = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<Ref<Resource>> bb;
    uint32_t bb_n = 0;
    uint32_t image = ~0u;  // acquired
    std::vector<VkSemaphore> acquire_sems, present_sems;
    uint32_t acquire_next = 0;
    VkSemaphore pending_acquire = VK_NULL_HANDLE;
    bool vsync = true, mailbox = false, immediate = false;
    VkExtent2D sc_extent{};
    // the main queue
    VkSemaphore timeline = VK_NULL_HANDLE;
    static constexpr uint32_t kSlots = 4;
    VkCommandPool pools[kSlots] = {};
    VkCommandBuffer cbs[kSlots] = {};
    // the slot's constant buffer sets of several buffers, by the buffers (pools added as they fill)
    std::vector<VkDescriptorPool> transient[kSlots];
    KeyMap<uint64_t, VkDescriptorSet> mixed[kSlots];
    VkDescriptorSet MixedSet(VkRes* const bufs[kCbvs]);
    std::vector<std::pair<VkQueryPool, uint32_t>> queries_used[kSlots];
    uint32_t slot = 0;
    VkList list{*this, true};
    VkUploader* uploader_wait = nullptr;
    uint64_t uploader_value = 0;
    // layouts
    VkDescriptorSetLayout set_layouts[2] = {};
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkSampler fixed_sampler = VK_NULL_HANDLE;
    VkSampler wrap_sampler = VK_NULL_HANDLE;  // s5: trilinear, repeating, no anisotropy
    VkSampler mat_sampler = VK_NULL_HANDLE;   // s6: the materials' maps, anisotropic at 4, repeating
    VkDescriptorPool ubo_pool = VK_NULL_HANDLE;
    std::mutex ubo_lock;
    VkPipelineCache cache = VK_NULL_HANDLE;
    // the views' tables
    std::vector<Slot> heaps[4];
    KeyMap<uint64_t, VkSampler> samplers;
    // a null view's: 1 x 1 zeros of each kind; a null vertex buffer's
    Ref<Resource> null_tex[kNullKinds];
    VkImageView null_view[kNullKinds] = {};
    Ref<Resource> null_buf;
    // buffers by number (GpuVa)
    std::vector<std::atomic<VkRes*>> buffers = std::vector<std::atomic<VkRes*>>(1u << 20);
    std::vector<uint32_t> free_ids;
    uint32_t next_id = 1;
    std::mutex id_lock;
    // the resources there are, by serial: a place in a table whose resource is gone reads as null
    // (each resource's serial at a place of its own, places reused: a view's check one atomic read, no lock, every
    // view of every draw checked)
    std::vector<std::atomic<uint64_t>> live = std::vector<std::atomic<uint64_t>>(1u << 20);
    std::vector<uint32_t> free_places;
    uint32_t next_place = 1;
    uint64_t next_serial = 1;
    std::mutex live_lock;
    void Register(VkRes* r) {
        std::lock_guard l(live_lock);
        r->serial = next_serial++;
        if (!free_places.empty()) r->place = free_places.back(), free_places.pop_back();
        else if (next_place < live.size()) r->place = next_place++;
        if (r->place) live[r->place].store(r->serial, std::memory_order_release);
    }
    void Unregister(VkRes* r) {
        if (!r->place) return;
        live[r->place].store(0, std::memory_order_release);
        std::lock_guard l(live_lock);
        free_places.push_back(r->place);
        r->place = 0;
    }
    VkRes* Alive(const Slot& s) {
        if (!s.res) return nullptr;
        if (s.place && live[s.place].load(std::memory_order_acquire) == s.serial) return s.res;
        LOG_FIRST(8, "vulkan: a view's resource is gone (serial %llu)", (unsigned long long)s.serial);
        return nullptr;
    }
    bool debug = false;
    // what released resources leave, kept until the GPU is past them
    struct Dead {
        uint64_t after = 0;
        std::vector<VkImageView> views;
        VkDescriptorSet ubo = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE;
        VkImage img = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        uint32_t id = 0;
    };
    std::deque<Dead> dead;
    std::mutex dead_lock;
    std::atomic<uint64_t> submitted{0};
    void Bury(Dead&& x) {
        x.after = submitted.load() + 1;
        std::lock_guard l(dead_lock);
        dead.push_back(std::move(x));
    }
    void Destroy(Dead& x) {
        for (VkImageView v : x.views) vkDestroyImageView(dev, v, nullptr);
        if (x.ubo) {
            std::lock_guard l(ubo_lock);
            vkFreeDescriptorSets(dev, ubo_pool, 1, &x.ubo);
        }
        if (x.buf) vmaDestroyBuffer(vma, x.buf, x.alloc);
        if (x.img) vmaDestroyImage(vma, x.img, x.alloc);
        if (x.id) {
            std::lock_guard l(id_lock);
            free_ids.push_back(x.id);
        }
    }
    void Collect(bool all) {
        uint64_t done = all ? UINT64_MAX : Completed();
        std::vector<Dead> now;
        {
            std::lock_guard l(dead_lock);
            while (!dead.empty() && dead.front().after <= done) now.push_back(std::move(dead.front())), dead.pop_front();
        }
        for (Dead& x : now) Destroy(x);
    }
    // XBE_VK_CHECKPOINTS: a marker after each command (VK_NV_device_diagnostic_checkpoints), what it was by number
    bool checkpoints = false;
    struct Mark {
        const char* what;
        VkPipe* pipe;
        Ref<Pipeline> keep;
    };
    std::vector<Mark> marks = std::vector<Mark>(1u << 16);
    std::atomic<uint32_t> mark_next{1};
    VkQueue upload_queue = VK_NULL_HANDLE;
    bool faulted = false;
    void Report();

    ~VkDev() override;
    const char* Name() const override { return "Vulkan"; }
    bool Vulkan() const override { return true; }
    bool Init(bool dbg);
    bool PickDevice();
    bool CreateLayouts();
    bool CreateNulls();

    bool CreateSwapChain(HWND h, uint32_t w, uint32_t hgt, uint32_t n) override;
    bool MakeSwapChain(uint32_t w, uint32_t h);
    bool Tearing() const override { return immediate || mailbox; }
    Resource* BackBuffer(uint32_t i) override { return i < bb.size() ? bb[i].Get() : nullptr; }
    uint32_t CurrentBackBuffer() override;
    bool ResizeSwapChain(uint32_t w, uint32_t h) override;
    void Present(bool vs) override;

    void CreateHeaps(const HeapSizes& s) override;
    bool CreateResource(HeapType heap, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE*,
                        Ref<Resource>& out) override;
    void CreateRtv(Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, View at) override;
    void CreateDsv(Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, View at) override;
    void CreateSrv(Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, View at) override;
    void CreateSampler(const D3D12_SAMPLER_DESC& d, View at) override;
    void Footprints(const D3D12_RESOURCE_DESC& d, uint32_t first, uint32_t n, uint64_t base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,
                    UINT* rows, UINT64* row_bytes, UINT64* total) override;
    bool SupportsSamples(DXGI_FORMAT f, uint32_t n) override;
    Ref<Blob> Compile(const std::string& src, const char* entry, const char* profile, std::string& error) override;
    Ref<Pipeline> CreatePipeline(const PipelineDesc& d) override;
    std::unique_ptr<PipelineCache> OpenPipelineCache(const std::vector<uint8_t>& data) override;
    const char* CacheTag() const override { return "vulkan"; }
    Ref<QueryPool> CreateTimestamps(uint32_t n) override;
    uint64_t TimestampFrequency() override { return uint64_t(1e9 / double(props.limits.timestampPeriod)); }
    List* MainList() override { return &list; }
    void Submit(uint64_t value) override;
    void Reset(uint32_t s) override;
    uint64_t Completed() override {
        uint64_t v = 0;
        if (vkGetSemaphoreCounterValue(dev, timeline, &v) != VK_SUCCESS) {
            Lost();
            return UINT64_MAX;  // (nothing more will finish: the waits end)
        }
        return v;
    }
    void Lost() {
        if (faulted) return;
        faulted = true;
        LOGE("vulkan: the device is lost");
        if (device_fault) {
            VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
            vkGetDeviceFaultInfoEXT(dev, &counts, nullptr);
            std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            info.pAddressInfos = addrs.data();
            info.pVendorInfos = vendor.data();
            counts.vendorBinarySize = 0;
            vkGetDeviceFaultInfoEXT(dev, &counts, &info);
            LOGE("vulkan: fault: %s", info.description);
            for (auto& a : addrs)
                LOGE("vulkan: fault: address type %d at %llx (precision %llx)", int(a.addressType), (unsigned long long)a.reportedAddress,
                     (unsigned long long)a.addressPrecision);
            for (auto& v : vendor)
                LOGE("vulkan: fault: %s (code %llx data %llx)", v.description, (unsigned long long)v.vendorFaultCode,
                     (unsigned long long)v.vendorFaultData);
        }
        if (checkpoints) Report();
    }
    void Wait(uint64_t value) override {
        if (Completed() >= value) return;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &value;
        if (VkResult r = vkWaitSemaphores(dev, &wi, UINT64_MAX); r != VK_SUCCESS) LOG_FIRST(8, "vulkan: wait %d", int(r));
    }
    void WaitUploader(Uploader* u, uint64_t value) override {
        uploader_wait = static_cast<VkUploader*>(u);
        uploader_value = std::max(uploader_value, value);
    }
    std::unique_ptr<Uploader> CreateUploader() override;

    VkRes* BufferOf(GpuVa va) const { return buffers[size_t(va >> 40)].load(std::memory_order_acquire); }
    // a constant buffer's range in it: 64 KB (an upload buffer has as much past its end)
    static VkDeviceSize CbvRange(const VkRes* r) {
        return std::min<VkDeviceSize>(kCbvRange, r->GetDesc().Width + (r->Heap() == HeapType::Upload ? kCbvRange : 0));
    }
    VkDescriptorSet UboSet(VkRes* r);
    VkSemaphore MakeSemaphore(bool timeline_kind);
    void OneShot(const std::function<void(VkCommandBuffer)>& f);
};

// ----------------------------------------------------------------------------------------------------------------
// resources

// gone for the renderer at once (its views' places read null); its Vulkan objects once the GPU is past every
// command recorded so far (VkDev::Collect)
VkRes::~VkRes() {
    d.Unregister(this);
    if (buf) d.buffers[id].store(nullptr, std::memory_order_release);
    if (pending) {
        auto& pc = d.list.pending_clears;
        pc.erase(std::remove(pc.begin(), pc.end(), this), pc.end());
    }
    VkDev::Dead x;
    for (auto& [k, v] : views) x.views.push_back(v);
    x.ubo = ubo;
    x.buf = buf;
    x.img = swapchain ? VK_NULL_HANDLE : img;
    x.alloc = alloc;
    x.id = buf ? id : 0;
    d.Bury(std::move(x));
}

uint8_t* VkRes::Map() {
    if (readback && map) vmaInvalidateAllocation(d.vma, alloc, 0, VK_WHOLE_SIZE);
    return map;
}

VkImageView VkRes::View(const ViewKey& k) {
    for (auto& [key, v] : views)
        if (key == k) return v;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = k.type;
    vi.format = k.format;
    vi.components = k.swizzle;
    vi.subresourceRange = {k.aspect, k.mip, k.mips, k.layer, k.layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(d.dev, &vi, nullptr, &v) != VK_SUCCESS) {
        LOGE("vulkan: image view failed (format %d type %d)", int(k.format), int(k.type));
        return VK_NULL_HANDLE;
    }
    views.push_back({k, v});
    return v;
}

VkPipe::~VkPipe() {
    if (p) vkDestroyPipeline(d.dev, p, nullptr);
}

VkQueries::~VkQueries() {
    if (pool) vkDestroyQueryPool(d.dev, pool, nullptr);
}

// ----------------------------------------------------------------------------------------------------------------
// the list

// tests (XBE_VK_SPLIT=<seconds>): from that time on, each draw run alone and waited for; the one the device is lost
// in named (its pipeline's shaders written to logs/split_<n>.spv)
const auto g_start = std::chrono::steady_clock::now();  // (the process's start, as the log counts)

void VkList::Split(uint32_t count) {
    static const double after = [] {
        const char* e = std::getenv("XBE_VK_SPLIT");
        return e ? std::atof(e) : -1.0;
    }();
    if (after < 0 || !main || d.faulted || std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() < after) return;
    LOG_FIRST(1, "vulkan: split: from here each draw alone");
    EndRendering();
    FlushBarriers();
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cb;
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    if (d.pending_acquire) {
        w.semaphore = d.pending_acquire;
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &w;
        d.pending_acquire = VK_NULL_HANDLE;
    }
    VkResult r;
    {
        std::lock_guard l(d.queue_lock);
        r = vkQueueSubmit2(d.queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(d.queue);
    }
    static uint64_t n = 0;
    n++;
    if (r != VK_SUCCESS) {
        LOGE("vulkan: split: the device is lost in draw %llu (pipeline %u, %u vertices or indices)", (unsigned long long)n,
             pipe_ ? pipe_->number : 0, count);
        for (size_t j = 0; pipe_ && j < pipe_->code.size(); j++) {
            auto path = app::ExeDir() / "logs" / ("split_" + std::to_string(j) + ".spv");
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                std::fwrite(pipe_->code[j].data(), 4, pipe_->code[j].size(), f);
                std::fclose(f);
            }
        }
        d.faulted = true;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = d.pools[d.slot];
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer next;
    vkAllocateCommandBuffers(d.dev, &ai, &next);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(next, &bi);
    cb = next;
    // (a new command buffer: its state set again)
    bound_pipe_ = nullptr;
    pipe_dirty_ = desc_dirty_ = cbv_dirty_ = true;
    vb_dirty_ = 0xFFFF;
    ib_dirty_ = true;
    if (dyn_ok_) {
        vkCmdSetViewport(cb, 0, 1, &vp_);
        vkCmdSetScissor(cb, 0, 1, &sc_);
        vkCmdSetBlendConstants(cb, bf_);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref_);
        vkCmdSetPrimitiveTopology(cb, topo_);
        dyn_set_ = 31;
    } else {
        dyn_set_ = 0;
    }
}

void VkList::Checkpoint(const char* what) {
    if (!d.checkpoints) return;
    uint32_t i = d.mark_next++ & 0xFFFF;
    if (!main) what = "uploader copy";
    d.marks[i] = {what, pipe_, Ref<Pipeline>(pipe_)};
    vkCmdSetCheckpointNV(cb, reinterpret_cast<void*>(uintptr_t(i)));
}

void VkList::Begin(VkCommandBuffer c) {
    cb = c;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    dyn_set_ = 0;
    pipe_ = bound_pipe_ = nullptr;
    pipe_dirty_ = desc_dirty_ = cbv_dirty_ = true;
    vb_dirty_ = 0xFFFF;
    ib_dirty_ = true;
    rendering_ = false;
    image_barriers_.clear();
    mem_barrier_ = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
}

void VkList::End() {
    FlushClears();
    EndRendering();
    FlushBarriers();
    vkEndCommandBuffer(cb);
}

void VkList::SetCbv(uint32_t param, GpuVa va) {
    uint32_t reg = layout_ == Layout::Main ? (param == 5 ? 2 : param) : param;
    if (reg >= kCbvs || cbv_[reg] == va) return;
    cbv_[reg] = va;
    cbv_dirty_ = true;
}

void VkList::SetTable(uint32_t param, Table t) {
    if (layout_ == Layout::Motion) {
        if (param == 5 && t_base_[0] != t.index) t_base_[0] = t.index, desc_dirty_ = true;
        return;
    }
    uint32_t& base = param == 2 ? t_base_[0] : param == 4 ? t_base_[1] : s_base_;
    if (param != 2 && param != 4 && param != 3) return;
    if (base == t.index) return;  // (the same table: what was pushed stands)
    base = t.index;
    desc_dirty_ = true;
}

void VkList::SetVertexBuffers(uint32_t start, uint32_t n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    for (uint32_t i = 0; i < n && start + i < 16; i++) {
        vb_[start + i] = v[i];
        vb_dirty_ |= 1u << (start + i);
    }
}

void VkList::SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) {
    ib_ = v ? *v : D3D12_INDEX_BUFFER_VIEW{};
    ib_dirty_ = true;
}

void VkList::SetTargets(uint32_t n, const View* rtv, const View* dsv) {
    bool same = n == rtv_n_ && bool(dsv) == has_dsv_ && (!dsv || *dsv == dsv_);
    for (uint32_t i = 0; same && i < n; i++) same = rtv[i] == rtv_[i];
    if (same) return;
    EndRendering("targets");
    rtv_n_ = std::min(n, 8u);
    for (uint32_t i = 0; i < rtv_n_; i++) rtv_[i] = rtv[i];
    has_dsv_ = dsv != nullptr;
    if (dsv) dsv_ = *dsv;
}

void VkList::ZeroFill(VkRes* r) {
    r->zero = false;
    EndRendering();
    FlushBarriers();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = r->use.stage ? r->use.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.srcAccessMask = r->use.access & kWrites;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r->img;
    b.subresourceRange = {r->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    if (r->fmt.depth) {
        VkClearDepthStencilValue z{0, 0};
        vkCmdClearDepthStencilImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &b.subresourceRange);
    } else {
        VkClearColorValue z{};
        vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &b.subresourceRange);
    }
    // the clear done before what comes next: a copy into the image would otherwise pass as a copy after copies
    // (Transition: no barrier between them) and race the clear (a write after write; seen after a preset change,
    // and Intel's Linux driver lost the device after one)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
    r->use = kCopyDst;
}

void VkList::Defer(VkRes* r, VkImageView view, VkExtent2D extent, VkImageAspectFlags aspect, const VkClearValue& v) {
    // (a depth clear and a stencil clear of one buffer: both)
    if (r->pending && r->pending_view == view) {
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) r->pending_value.depthStencil.depth = v.depthStencil.depth;
        if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) r->pending_value.depthStencil.stencil = v.depthStencil.stencil;
        if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) r->pending_value = v;
        r->pending_aspect |= aspect;
        return;
    }
    if (r->pending) FlushClear(r);
    pending_clears.push_back(r);
    r->pending = true;
    r->pending_aspect = aspect;
    r->pending_value = v;
    r->pending_view = view;
    r->pending_extent = extent;
}

// a deferred clear done now, in a rendering instance of its own
void VkList::FlushClear(VkRes* r) {
    if (!r->pending) return;
    r->pending = false;
    pending_clears.erase(std::remove(pending_clears.begin(), pending_clears.end(), r), pending_clears.end());
    EndRendering();
    FlushBarriers();
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, st{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = st.imageView = r->pending_view;
    a.storeOp = st.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue = st.clearValue = r->pending_value;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, r->pending_extent};
    ri.layerCount = 1;
    if (r->pending_aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
    } else {
        a.imageLayout = st.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.loadOp = r->pending_aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        st.loadOp = r->pending_aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ri.pDepthAttachment = &a;
        if (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &st;
    }
    vkCmdBeginRendering(cb, &ri);
    vkCmdEndRendering(cb);
    clears++;
}

void VkList::FlushClears() {
    while (!pending_clears.empty()) FlushClear(pending_clears.back());
}

void VkList::Transition(VkRes* r, const Use& to, bool keep_clear) {
    if (!r) return;
    if (r->zero && main) ZeroFill(r);
    if (r->pending && !keep_clear) FlushClear(r);
    Use& from = r->use;
    if (r->img) {
        bool same = from.layout == to.layout;
        // read after read in one layout, or copies after copies (other subresources: the uploads' levels): nothing
        if (same && !(from.access & kWrites)) {
            from.stage |= to.stage, from.access |= to.access;
            return;
        }
        if (same && from.access == VK_ACCESS_2_TRANSFER_WRITE_BIT && to.access == VK_ACCESS_2_TRANSFER_WRITE_BIT) return;
        if (dump && rendering_)
            LOGR("vulkan: frame: barrier on %ux%u format %d samples %u, layout %d to %d", r->extent.width, r->extent.height,
                 int(r->format), r->samples, int(from.layout), int(to.layout));
        EndRendering("image barrier");
        // (one barrier call's transitions are not in order among themselves: the image's last one goes first)
        for (const VkImageMemoryBarrier2& b : image_barriers_)
            if (b.image == r->img) {
                FlushBarriers();
                break;
            }
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = from.stage ? from.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.srcAccessMask = from.access & kWrites;
        b.dstStageMask = to.stage ? to.stage : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        b.dstAccessMask = to.access;
        b.oldLayout = from.layout;
        b.newLayout = to.layout;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r->img;
        b.subresourceRange = {r->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        image_barriers_.push_back(b);
        from = to;
        return;
    }
    // buffers: a memory barrier for writes before
    if (from.access & kWrites) {
        EndRendering("buffer barrier");
        mem_barrier_.srcStageMask |= from.stage;
        mem_barrier_.srcAccessMask |= from.access & kWrites;
        mem_barrier_.dstStageMask |= to.stage;
        mem_barrier_.dstAccessMask |= to.access;
    }
    from = to;
}

void VkList::FlushBarriers() {
    if (image_barriers_.empty() && !mem_barrier_.srcStageMask) return;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = uint32_t(image_barriers_.size());
    di.pImageMemoryBarriers = image_barriers_.data();
    if (mem_barrier_.srcStageMask) di.memoryBarrierCount = 1, di.pMemoryBarriers = &mem_barrier_;
    vkCmdPipelineBarrier2(cb, &di);
    barriers++;
    image_barriers_.clear();
    mem_barrier_ = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
}

void VkList::EndRendering(const char* why) {
    if (!rendering_) return;
    vkCmdEndRendering(cb);
    rendering_ = false;
    if (dump) LOGR("vulkan: frame: pass ends (%s) after %u draws", why, pass_draws_);
}

bool VkList::Attachments(VkRenderingAttachmentInfo* colors, uint32_t& nc, VkRenderingAttachmentInfo& depth, bool& has_depth,
                         VkExtent2D& area) {
    area = {UINT32_MAX, UINT32_MAX};
    nc = 0;
    for (uint32_t i = 0; i < rtv_n_; i++) {
        const Slot& s = d.heaps[int(Heap::Rtv)][rtv_[i].index];
        VkRenderingAttachmentInfo& a = colors[nc++];
        a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!d.Alive(s) || !s.view) continue;
        a.imageView = s.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (s.res->pending && s.res->pending_view == s.view) a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, a.clearValue = s.res->pending_value;
        area.width = std::min(area.width, s.extent.width), area.height = std::min(area.height, s.extent.height);
    }
    has_depth = false;
    if (has_dsv_) {
        const Slot& s = d.heaps[int(Heap::Dsv)][dsv_.index];
        if (d.Alive(s) && s.view) {
            depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = s.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            stencil_ = depth;
            if (s.res->pending && s.res->pending_view == s.view) {
                depth.clearValue = stencil_.clearValue = s.res->pending_value;
                if (s.res->pending_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                if (s.res->pending_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencil_.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
            has_depth = true;
            area.width = std::min(area.width, s.extent.width), area.height = std::min(area.height, s.extent.height);
        }
    }
    return area.width != UINT32_MAX;
}

void VkList::BeginRendering() {
    // the targets' layouts first (a target made in the render target state, its writes from an earlier instance)
    for (uint32_t i = 0; i < rtv_n_; i++) {
        const Slot& s = d.heaps[int(Heap::Rtv)][rtv_[i].index];
        if (d.Alive(s)) Transition(s.res, kRender, s.res->pending && s.res->pending_view == s.view);
    }
    if (has_dsv_) {
        const Slot& s = d.heaps[int(Heap::Dsv)][dsv_.index];
        if (d.Alive(s)) Transition(s.res, kDepthWrite, s.res->pending && s.res->pending_view == s.view);
    }
    FlushBarriers();
    VkRenderingAttachmentInfo colors[8], depth{};
    uint32_t nc = 0;
    bool has_depth = false;
    VkExtent2D area;
    if (!Attachments(colors, nc, depth, has_depth, area)) {
        LOG_FIRST(8, "vulkan: a draw without targets (%u colour, depth %d)", rtv_n_, int(has_dsv_));
        return;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, area};
    ri.layerCount = 1;
    ri.colorAttachmentCount = nc;
    ri.pColorAttachments = colors;
    if (has_depth) {
        ri.pDepthAttachment = &depth;
        if (d.heaps[int(Heap::Dsv)][dsv_.index].aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil_;
    }
    vkCmdBeginRendering(cb, &ri);
    rendering_ = true;
    passes++;
    if (dump) LOGR("vulkan: frame: pass begins, %u colour (%d %ux%u x%u), depth %d", nc, nc ? int(colors[0].imageView != VK_NULL_HANDLE) : 0,
                   area.width, area.height, nc ? d.heaps[int(Heap::Rtv)][rtv_[0].index].res->samples : 0, int(has_depth));
    pass_draws_ = 0;
    // (the deferred clears the load did)
    auto done = [&](VkRes* r, VkImageView v) {
        if (!r || !r->pending || r->pending_view != v) return;
        r->pending = false;
        pending_clears.erase(std::remove(pending_clears.begin(), pending_clears.end(), r), pending_clears.end());
    };
    for (uint32_t i = 0; i < rtv_n_; i++) done(d.Alive(d.heaps[int(Heap::Rtv)][rtv_[i].index]), colors[i].imageView);
    if (has_dsv_ && has_depth) done(d.Alive(d.heaps[int(Heap::Dsv)][dsv_.index]), depth.imageView);
    // (what is drawn next writes them)
    for (uint32_t i = 0; i < rtv_n_; i++)
        if (VkRes* r = d.Alive(d.heaps[int(Heap::Rtv)][rtv_[i].index])) r->use.access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_dsv_)
        if (VkRes* r = d.Alive(d.heaps[int(Heap::Dsv)][dsv_.index])) r->use.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}

VkImageView VkList::SrvView(uint32_t slot, uint8_t kind_if_null, VkImageLayout& layout) {
    const auto& heap = d.heaps[int(Heap::Srv)];
    const Slot* s = slot < heap.size() ? &heap[slot] : nullptr;
    if (!s || !d.Alive(*s) || !s->view) {
        layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return d.null_view[s ? s->null_kind : kind_if_null];
    }
    layout = s->res->use.layout;
    return s->view;
}

void VkList::PushDescriptors() {
    const VkPipe& p = *pipe_;
    VkDescriptorImageInfo ii[kTextures + kSamplers];
    VkWriteDescriptorSet ws[kTextures + kSamplers];
    uint32_t n = 0;
    for (uint32_t t = 0; t < kTextures; t++) {
        if (!(p.textures >> t & 1)) continue;
        uint32_t base = t < 7 ? t_base_[0] : t_base_[1];
        uint32_t at = base == ~0u ? ~0u : base + (t < 7 ? t : t - 7);
        ii[n] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[n].imageView = SrvView(at, kNull2D, ii[n].imageLayout);
        pushed_layout_[t] = ii[n].imageLayout;
        ws[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[n].dstBinding = kTBinding + t;
        ws[n].descriptorCount = 1;
        ws[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ws[n].pImageInfo = &ii[n];
        n++;
    }
    for (uint32_t i = 0; i < kSamplers; i++) {
        if (!(p.samplers >> i & 1)) continue;
        const auto& heap = d.heaps[int(Heap::Sampler)];
        VkSampler smp = s_base_ != ~0u && s_base_ + i < heap.size() ? heap[s_base_ + i].sampler : VK_NULL_HANDLE;
        if (!smp) smp = d.fixed_sampler;
        ii[n] = {smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        ws[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[n].dstBinding = kSBinding + i;
        ws[n].descriptorCount = 1;
        ws[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        ws[n].pImageInfo = &ii[n];
        n++;
    }
    if (n) vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 0, n, ws);
}

// the constant buffers the pipeline's shaders read, by dynamic offsets into the set of the buffer they lie in; ones in
// several buffers a set of the frame's own, made once a frame for those buffers (the ones the shaders do not read:
// offset 0 of the first's, whatever was bound there before)
void VkList::BindCbvs() {
    const uint32_t used = pipe_->cbvs;
    if (!used) return;
    VkRes* bufs[kCbvs] = {};
    VkRes* first = nullptr;
    for (uint32_t i = 0; i < kCbvs; i++) {
        if (!(used >> i & 1) || !cbv_[i]) continue;
        bufs[i] = d.BufferOf(cbv_[i]);
        if (!first) first = bufs[i];
    }
    if (!first) return;
    bool one = true;
    uint32_t offsets[kCbvs];
    for (uint32_t i = 0; i < kCbvs; i++) {
        offsets[i] = bufs[i] ? uint32_t(cbv_[i] & ((1ull << 40) - 1)) : 0;
        if (!bufs[i]) bufs[i] = first;
        one = one && bufs[i] == first;
    }
    VkDescriptorSet set = one ? d.UboSet(first) : d.MixedSet(bufs);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 1, 1, &set, kCbvs, offsets);
}

bool VkList::Prepare() {
    static const char* skip = std::getenv("XBE_VK_SKIP");
    if (skip && pipe_ && ((pipe_->gs && std::strstr(skip, "gs")) || (pipe_->tess && std::strstr(skip, "tess")))) return false;
    if (!pipe_ || !pipe_->p) {
        LOG_FIRST(8, "vulkan: a draw without a pipeline");
        return false;
    }
    // the textures the shaders read: readable (a texture the uploader filled, one left in another layout), which a
    // rendering instance may not be in for
    // pushed again when the tables moved, the pipeline reads a binding not pushed since, or a texture's layout is not
    // the one pushed (the push descriptors stay through pipelines of the one layout: most draws push nothing)
    bool push = desc_dirty_ || (pipe_->textures & ~pushed_t_) || (pipe_->samplers & ~pushed_s_);
    for (uint32_t t = 0; t < kTextures; t++) {
        if (!(pipe_->textures >> t & 1)) continue;
        uint32_t base = t < 7 ? t_base_[0] : t_base_[1];
        if (base == ~0u) continue;
        uint32_t at = base + (t < 7 ? t : t - 7);
        const auto& heap = d.heaps[int(Heap::Srv)];
        if (at >= heap.size()) continue;
        VkRes* r = d.Alive(heap[at]);
        if (!r) continue;
        if (push)
            for (uint32_t i = 0; i < rtv_n_ + (has_dsv_ ? 1 : 0); i++) {
                const Slot& a = i < rtv_n_ ? d.heaps[int(Heap::Rtv)][rtv_[i].index] : d.heaps[int(Heap::Dsv)][dsv_.index];
                if (a.res == r) LOG_FIRST(8, "vulkan: a draw reads t%u from its own target (format %d)", t, int(r->format));
            }
        if (!Readable(r->use.layout)) Transition(r, kSampled);
        if (pushed_layout_[t] != r->use.layout) push = true;
    }
    if (!rendering_) {
        BeginRendering();
        if (!rendering_) return false;
    } else if (!image_barriers_.empty() || mem_barrier_.srcStageMask) {
        EndRendering("draw's barriers");
        BeginRendering();
        if (!rendering_) return false;
    }
    static const bool trace = std::getenv("XBE_VK_TRACE") != nullptr;
    if (trace && !pipe_->drawn) {
        pipe_->drawn = true;
        LOGR("vulkan: pipeline %u first drawn (stages %zu)", pipe_->number, pipe_->code.size());
        for (size_t j = 0; j < pipe_->code.size(); j++) {
            auto path = app::ExeDir() / "logs" / ("pipe_" + std::to_string(pipe_->number) + "_" + std::to_string(j) + ".spv");
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                std::fwrite(pipe_->code[j].data(), 4, pipe_->code[j].size(), f);
                std::fclose(f);
            }
        }
    }
    if (pipe_dirty_ || pipe_ != bound_pipe_) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_->p);
        bound_pipe_ = pipe_;
        pipe_dirty_ = false;
    }
    if (push) {
        PushDescriptors();
        pushed_t_ = desc_dirty_ ? pipe_->textures : pushed_t_ | pipe_->textures;
        pushed_s_ = desc_dirty_ ? pipe_->samplers : pushed_s_ | pipe_->samplers;
        desc_dirty_ = false;
    }
    if (cbv_dirty_) {
        BindCbvs();
        cbv_dirty_ = false;
    }
    if (vb_dirty_) {
        VkBuffer b[16];
        VkDeviceSize o[16], sz[16], st[16];
        for (uint32_t i = 0; i < 16; i++) {
            if (!(vb_dirty_ >> i & 1)) continue;
            uint32_t j = i;
            uint32_t n = 0;
            for (; j < 16 && (vb_dirty_ >> j & 1); j++, n++) {
                const D3D12_VERTEX_BUFFER_VIEW& v = vb_[j];
                VkRes* r = v.BufferLocation ? d.BufferOf(v.BufferLocation) : nullptr;
                if (!r) r = static_cast<VkRes*>(d.null_buf.Get());
                b[n] = r->buf;
                o[n] = v.BufferLocation ? v.BufferLocation & ((1ull << 40) - 1) : 0;
                sz[n] = v.BufferLocation ? v.SizeInBytes : r->GetDesc().Width;
                st[n] = v.BufferLocation ? v.StrideInBytes : 0;
            }
            vkCmdBindVertexBuffers2(cb, i, n, b, o, sz, st);
            i = j;
        }
        vb_dirty_ = 0;
    }
    if (ib_dirty_ && ib_.BufferLocation) {
        if (VkRes* r = d.BufferOf(ib_.BufferLocation))
            vkCmdBindIndexBuffer(cb, r->buf, ib_.BufferLocation & ((1ull << 40) - 1),
                                 ib_.Format == DXGI_FORMAT_R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        ib_dirty_ = false;
    }
    return true;
}

// a clear of a view: inside the rendering instance that has it, else one of its own
void VkList::ClearRtv(View v, const float color[4], uint32_t n, const D3D12_RECT* rects) {
    const Slot& s = d.heaps[int(Heap::Rtv)][v.index];
    if (!d.Alive(s) || !s.view) return;
    bool inside = false;
    if (rendering_)
        for (uint32_t i = 0; i < rtv_n_; i++)
            if (rtv_[i] == v) inside = true;
    if (!inside && !n && main) {
        // whole: the next rendering instance's load (Defer)
        Transition(s.res, kRender, true);
        VkClearValue cv{};
        std::memcpy(cv.color.float32, color, 16);
        Defer(s.res, s.view, s.extent, VK_IMAGE_ASPECT_COLOR_BIT, cv);
        return;
    }
    if (!inside) {
        EndRendering();
        Transition(s.res, kRender);
        FlushBarriers();
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = s.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = n ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(a.clearValue.color.float32, color, 16);
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, s.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
        vkCmdBeginRendering(cb, &ri);
        clears++;
        s.res->use.access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (n || inside) {
        uint32_t index = 0;
        for (uint32_t i = 0; inside && i < rtv_n_; i++)
            if (rtv_[i] == v) index = i;
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, index};
        std::memcpy(ca.clearValue.color.float32, color, 16);
        std::vector<VkClearRect> cr;
        if (!n) cr.push_back({{{0, 0}, s.extent}, 0, 1});
        for (uint32_t i = 0; i < n; i++) {
            LONG x0 = std::clamp<LONG>(rects[i].left, 0, LONG(s.extent.width)), y0 = std::clamp<LONG>(rects[i].top, 0, LONG(s.extent.height));
            LONG x1 = std::clamp<LONG>(rects[i].right, 0, LONG(s.extent.width)), y1 = std::clamp<LONG>(rects[i].bottom, 0, LONG(s.extent.height));
            if (x1 > x0 && y1 > y0) cr.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
        }
        if (!cr.empty()) vkCmdClearAttachments(cb, 1, &ca, uint32_t(cr.size()), cr.data());
    }
    if (!inside) vkCmdEndRendering(cb);
}

void VkList::ClearDsv(View v, D3D12_CLEAR_FLAGS flags, float z, uint8_t stencil, uint32_t n, const D3D12_RECT* rects) {
    const Slot& s = d.heaps[int(Heap::Dsv)][v.index];
    if (!d.Alive(s) || !s.view) return;
    VkImageAspectFlags aspect = ((flags & D3D12_CLEAR_FLAG_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                ((flags & D3D12_CLEAR_FLAG_STENCIL) ? (s.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) : 0);
    if (!aspect) return;
    bool inside = rendering_ && has_dsv_ && dsv_ == v;
    if (!inside && !n && main) {
        Transition(s.res, kDepthWrite, true);
        VkClearValue cv{};
        cv.depthStencil = {z, stencil};
        Defer(s.res, s.view, s.extent, aspect, cv);
        return;
    }
    if (!inside) {
        EndRendering();
        Transition(s.res, kDepthWrite);
        FlushBarriers();
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = s.view;
        a.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo st = a;
        if (!n && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        if (!n && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) st.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.clearValue.depthStencil = st.clearValue.depthStencil = {z, stencil};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, s.extent};
        ri.layerCount = 1;
        ri.pDepthAttachment = &a;
        if (s.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &st;
        vkCmdBeginRendering(cb, &ri);
        clears++;
        s.res->use.access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (n || inside) {
        VkClearAttachment ca{aspect, 0};
        ca.clearValue.depthStencil = {z, stencil};
        std::vector<VkClearRect> cr;
        if (!n) cr.push_back({{{0, 0}, s.extent}, 0, 1});
        for (uint32_t i = 0; i < n; i++) {
            LONG x0 = std::clamp<LONG>(rects[i].left, 0, LONG(s.extent.width)), y0 = std::clamp<LONG>(rects[i].top, 0, LONG(s.extent.height));
            LONG x1 = std::clamp<LONG>(rects[i].right, 0, LONG(s.extent.width)), y1 = std::clamp<LONG>(rects[i].bottom, 0, LONG(s.extent.height));
            if (x1 > x0 && y1 > y0) cr.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
        }
        if (!cr.empty()) vkCmdClearAttachments(cb, 1, &ca, uint32_t(cr.size()), cr.data());
    }
    if (!inside) vkCmdEndRendering(cb);
}

// a footprint's region of a subresource, as VkBufferImageCopy2
VkBufferImageCopy2 Region(const VkRes& img, uint32_t sub, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, uint32_t x, uint32_t y,
                          uint32_t z, const D3D12_BOX* box) {
    uint32_t mip = sub % img.mips, layer = sub / img.mips;
    Fmt f = FormatOf(fp.Footprint.Format);
    if (!f.bytes) f = img.fmt;
    VkBufferImageCopy2 r{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    r.bufferOffset = fp.Offset;
    r.bufferRowLength = fp.Footprint.RowPitch / f.bytes * f.block;
    r.bufferImageHeight = (fp.Footprint.Height + f.block - 1) / f.block * f.block;
    VkImageAspectFlags aspect = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : img.aspect;
    r.imageSubresource = {aspect, mip, img.is3d ? 0 : layer, 1};
    uint32_t mw = std::max(1u, img.extent.width >> mip), mh = std::max(1u, img.extent.height >> mip);
    uint32_t md = img.is3d ? std::max(1u, img.extent.depth >> mip) : 1;
    uint32_t w = fp.Footprint.Width, h = fp.Footprint.Height, dp = fp.Footprint.Depth;
    if (box) {
        r.bufferOffset += uint64_t(box->top / f.block) * fp.Footprint.RowPitch + uint64_t(box->left / f.block) * f.bytes;
        w = box->right - box->left, h = box->bottom - box->top, dp = box->back - box->front;
    }
    r.imageOffset = {int32_t(x), int32_t(y), int32_t(z)};
    r.imageExtent = {std::min(w, mw - std::min(mw, x)), std::min(h, mh - std::min(mh, y)), std::max(1u, std::min(dp, md - std::min(md, z)))};
    return r;
}

void VkList::CopyTexture(const CopyLocation& dst, uint32_t x, uint32_t y, uint32_t z, const CopyLocation& src, const D3D12_BOX* box) {
    auto* dr = static_cast<VkRes*>(dst.pResource);
    auto* sr = static_cast<VkRes*>(src.pResource);
    if (!dr || !sr) return;
    EndRendering(dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? "readback copy" : "texture copy");
    if (dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        if (dr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) Transition(dr, kCopyDst);
        Transition(sr, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
        FlushBarriers();
        VkBufferImageCopy2 r = Region(*dr, dst.SubresourceIndex, src.PlacedFootprint, x, y, z, box);
        if (!r.imageExtent.width || !r.imageExtent.height) return;
        VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ci.srcBuffer = sr->buf;
        ci.dstImage = dr->img;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &r;
        vkCmdCopyBufferToImage2(cb, &ci);
        Checkpoint("copy buffer to image");
        dr->use.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (!main && std::find(uploaded.begin(), uploaded.end(), dr) == uploaded.end()) uploaded.push_back(dr);
    } else if (dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        if (sr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) Transition(sr, kCopySrc);
        Transition(dr, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
        FlushBarriers();
        VkBufferImageCopy2 r = Region(*sr, src.SubresourceIndex, dst.PlacedFootprint, 0, 0, 0, nullptr);
        if (box) r.imageOffset = {int32_t(box->left), int32_t(box->top), int32_t(box->front)},
                 r.imageExtent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
        VkCopyImageToBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        ci.srcImage = sr->img;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstBuffer = dr->buf;
        ci.regionCount = 1;
        ci.pRegions = &r;
        vkCmdCopyImageToBuffer2(cb, &ci);
        Checkpoint("copy image to buffer");
    } else {
        if (sr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) Transition(sr, kCopySrc);
        if (dr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) Transition(dr, kCopyDst);
        FlushBarriers();
        uint32_t smip = src.SubresourceIndex % sr->mips, slayer = src.SubresourceIndex / sr->mips;
        uint32_t dmip = dst.SubresourceIndex % dr->mips, dlayer = dst.SubresourceIndex / dr->mips;
        VkImageCopy2 r{VK_STRUCTURE_TYPE_IMAGE_COPY_2};
        r.srcSubresource = {sr->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sr->aspect, smip, slayer, 1};
        r.dstSubresource = {dr->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : dr->aspect, dmip, dlayer, 1};
        uint32_t sw = std::max(1u, sr->extent.width >> smip), sh = std::max(1u, sr->extent.height >> smip);
        r.srcOffset = box ? VkOffset3D{int32_t(box->left), int32_t(box->top), int32_t(box->front)} : VkOffset3D{};
        r.extent = box ? VkExtent3D{box->right - box->left, box->bottom - box->top, std::max(1u, box->back - box->front)} : VkExtent3D{sw, sh, 1};
        r.dstOffset = {int32_t(x), int32_t(y), int32_t(z)};
        VkCopyImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
        ci.srcImage = sr->img;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstImage = dr->img;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &r;
        vkCmdCopyImage2(cb, &ci);
        dr->use.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        Checkpoint("copy image");
    }
}

void VkList::CopyBuffer(Resource* dst, uint64_t dst_offset, Resource* src, uint64_t src_offset, uint64_t bytes) {
    auto* dr = static_cast<VkRes*>(dst);
    auto* sr = static_cast<VkRes*>(src);
    EndRendering();
    Transition(dr, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    Transition(sr, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
    FlushBarriers();
    VkBufferCopy2 r{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, src_offset, dst_offset, bytes};
    VkCopyBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    ci.srcBuffer = sr->buf;
    ci.dstBuffer = dr->buf;
    ci.regionCount = 1;
    ci.pRegions = &r;
    vkCmdCopyBuffer2(cb, &ci);
}

void VkList::CopyResource(Resource* dst, Resource* src) {
    auto* dr = static_cast<VkRes*>(dst);
    auto* sr = static_cast<VkRes*>(src);
    if (dr->buf) {
        CopyBuffer(dst, 0, src, 0, std::min(dr->GetDesc().Width, sr->GetDesc().Width));
        return;
    }
    EndRendering();
    if (sr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) Transition(sr, kCopySrc);
    if (dr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) Transition(dr, kCopyDst);
    FlushBarriers();
    std::vector<VkImageCopy2> rs;
    for (uint32_t m = 0; m < std::min(sr->mips, dr->mips); m++) {
        VkImageCopy2 r{VK_STRUCTURE_TYPE_IMAGE_COPY_2};
        VkImageAspectFlags sa = sr->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sr->aspect;
        r.srcSubresource = {sa, m, 0, std::min(sr->layers, dr->layers)};
        r.dstSubresource = {sa, m, 0, std::min(sr->layers, dr->layers)};
        r.extent = {std::max(1u, std::min(sr->extent.width, dr->extent.width) >> m), std::max(1u, std::min(sr->extent.height, dr->extent.height) >> m),
                    sr->is3d ? std::max(1u, sr->extent.depth >> m) : 1};
        rs.push_back(r);
    }
    VkCopyImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    ci.srcImage = sr->img;
    ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ci.dstImage = dr->img;
    ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ci.regionCount = uint32_t(rs.size());
    ci.pRegions = rs.data();
    vkCmdCopyImage2(cb, &ci);
    dr->use.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

void VkList::Resolve(Resource* dst, Resource* src, DXGI_FORMAT) {
    auto* dr = static_cast<VkRes*>(dst);
    auto* sr = static_cast<VkRes*>(src);
    EndRendering();
    if (sr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) Transition(sr, kCopySrc);
    if (dr->use.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) Transition(dr, kCopyDst);
    FlushBarriers();
    VkImageResolve2 r{VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {std::min(sr->extent.width, dr->extent.width), std::min(sr->extent.height, dr->extent.height), 1};
    VkResolveImageInfo2 ri{VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2};
    ri.srcImage = sr->img;
    ri.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ri.dstImage = dr->img;
    ri.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ri.regionCount = 1;
    ri.pRegions = &r;
    vkCmdResolveImage2(cb, &ri);
    Checkpoint("resolve");
    dr->use.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

void VkList::Timestamp(QueryPool* q, uint32_t i) {
    auto* p = static_cast<VkQueries*>(q);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p->pool, i);
    if (main) d.queries_used[d.slot].push_back({p->pool, i});
}

void VkList::ResolveTimestamps(QueryPool* q, uint32_t first, uint32_t n, Resource* dst, uint64_t offset) {
    auto* p = static_cast<VkQueries*>(q);
    auto* dr = static_cast<VkRes*>(dst);
    EndRendering();
    Transition(dr, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    FlushBarriers();
    vkCmdCopyQueryPoolResults(cb, p->pool, first, n, dr->buf, offset, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    Checkpoint("query results");
}

void VkList::Acquire(Resource* r) {
    auto* x = static_cast<VkRes*>(r);
    if (!x || !x->acquire) return;
    x->acquire = false;
    if (d.copy_family == ~0u || d.copy_family == d.family) return;
    // the uploader's release, matched: another queue family's
    EndRendering();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = kShaders;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = d.copy_family;
    b.dstQueueFamilyIndex = d.family;
    b.image = x->img;
    b.subresourceRange = {x->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    image_barriers_.push_back(b);
    x->use = kSampled;
}

// ----------------------------------------------------------------------------------------------------------------
// the uploader

VkUploader::~VkUploader() {
    vkDeviceWaitIdle(d.dev);
    for (auto p : pool)
        if (p) vkDestroyCommandPool(d.dev, p, nullptr);
    if (timeline) vkDestroySemaphore(d.dev, timeline, nullptr);
}

List* VkUploader::Begin(uint32_t s) {
    slot = s & 1;
    vkResetCommandPool(d.dev, pool[slot], 0);
    list.uploaded.clear();
    list.Begin(cbs[slot]);
    return &list;
}

uint64_t VkUploader::Submit() {
    // the textures it filled readable, given over to the main queue's family when it is another
    bool other = family != d.family;
    for (VkRes* r : list.uploaded) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = other ? family : VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = other ? d.family : VK_QUEUE_FAMILY_IGNORED;
        b.image = r->img;
        b.subresourceRange = {r->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(list.cb, &di);
        r->use = {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        r->acquire = true;
    }
    list.End();
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = list.cb;
    VkSemaphoreSubmitInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    si.semaphore = timeline;
    si.value = ++value;
    si.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 sub{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    sub.commandBufferInfoCount = 1;
    sub.pCommandBufferInfos = &ci;
    sub.signalSemaphoreInfoCount = 1;
    sub.pSignalSemaphoreInfos = &si;
    if (queue_lock) {
        std::lock_guard l(*queue_lock);
        vkQueueSubmit2(queue, 1, &sub, VK_NULL_HANDLE);
    } else {
        vkQueueSubmit2(queue, 1, &sub, VK_NULL_HANDLE);
    }
    return value;
}

uint64_t VkUploader::Completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(d.dev, timeline, &v);
    return v;
}

void VkUploader::Wait(uint64_t v) {
    if (Completed() >= v) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    vkWaitSemaphores(d.dev, &wi, UINT64_MAX);
}

// ----------------------------------------------------------------------------------------------------------------
// the device

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    static std::mutex m;
    static std::unordered_map<int32_t, int> told;
    std::lock_guard l(m);
    if (told[data->messageIdNumber]++ >= 3) return VK_FALSE;
    bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    if (error) LOGE("vulkan validation: %s", data->pMessage);
    else LOGR("vulkan validation: %s", data->pMessage);
    return VK_FALSE;
}

bool VkDev::Init(bool dbg) {
    debug = dbg;
    if (volkInitialize() != VK_SUCCESS) {
        LOGE("vulkan: no Vulkan loader (vulkan-1.dll)");
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        LOGE("vulkan: the loader is older than Vulkan 1.3");
        return false;
    }
    std::vector<const char*> layers, exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (debug) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        for (auto& l : lp)
            if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) LOGR("vulkan: the validation layer is not installed (VK_ADD_LAYER_PATH)");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Finest Hour Remastered";
    app.pEngineName = "xbe-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        LOGE("vulkan: no instance");
        return false;
    }
    volkLoadInstance(instance);
    if (debug) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = OnMessage;
        vkCreateDebugUtilsMessengerEXT(instance, &mi, nullptr, &messenger);
    }
    hwnd = static_cast<HWND>(app::Window());
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = hwnd;
    if (vkCreateWin32SurfaceKHR(instance, &si, nullptr, &surface) != VK_SUCCESS) {
        LOGE("vulkan: no surface");
        return false;
    }
    if (!PickDevice()) return false;
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = phys;
    ai.device = dev;
    ai.instance = instance;
    ai.pVulkanFunctions = &vf;
    if (vmaCreateAllocator(&ai, &vma) != VK_SUCCESS) return false;
    timeline = MakeSemaphore(true);
    for (uint32_t i = 0; i < kSlots; i++) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family;
        vkCreateCommandPool(dev, &pi, nullptr, &pools[i]);
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = pools[i];
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ci, &cbs[i]);
    }
    if (!CreateLayouts()) return false;
    list.Begin(cbs[0]);
    return CreateNulls();
}

bool VkDev::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(instance, &n, all.data());
    int best = -1;
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(p, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ep(ne);
        vkEnumerateDeviceExtensionProperties(p, nullptr, &ne, ep.data());
        auto has = [&](const char* e) {
            for (auto& x : ep)
                if (!std::strcmp(x.extensionName, e)) return true;
            return false;
        };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > best)
            best = score, phys = p, props = pr, custom_border = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME),
            device_fault = has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    if (!phys) {
        LOGE("vulkan: no device with Vulkan 1.3, swap chains and push descriptors");
        return false;
    }
    LOGR("Vulkan device: %s (driver %u.%u.%u, Vulkan %u.%u)", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
         VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion), VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion));
    // queues: graphics that presents to the window; a transfer queue of its own for the uploader
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qp(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp.data());
    family = ~0u;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            family = i;
            break;
        }
    }
    if (family == ~0u) return false;
    for (uint32_t i = 0; i < nq; i++) {
        const VkExtent3D& g = qp[i].minImageTransferGranularity;
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            g.width == 1 && g.height == 1 && g.depth == 1) {
            copy_family = i;
            break;
        }
    }
    if (copy_family == ~0u && qp[family].queueCount > 1) copy_family = family, copy_index = 1;
    // tests: XBE_VK_COPY_QUEUE=0 the uploads on the main queue, 1 on a second graphics queue
    if (const char* e = std::getenv("XBE_VK_COPY_QUEUE")) {
        if (*e == '0') copy_family = ~0u;
        else if (*e == '1') copy_family = qp[family].queueCount > 1 ? family : ~0u, copy_index = 1;
    }
    float prio[2] = {1.0f, 0.5f};
    VkDeviceQueueCreateInfo qi[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    qi[0].queueFamilyIndex = family;
    qi[0].queueCount = copy_family == family ? 2 : 1;
    qi[0].pQueuePriorities = prio;
    qi[1].queueFamilyIndex = copy_family;
    qi[1].queueCount = 1;
    qi[1].pQueuePriorities = prio + 1;
    uint32_t nqi = copy_family != ~0u && copy_family != family ? 2 : 1;
    // the features the renderer's shaders and states take
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    f2.pNext = &f12;
    f12.pNext = &f13;
    if (custom_border) f13.pNext = &fcb;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features w12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features w13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT wcb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    want.pNext = &w12;
    w12.pNext = &w13;
    const VkPhysicalDeviceFeatures& h = f2.features;
    VkPhysicalDeviceFeatures& w = want.features;
    w.geometryShader = h.geometryShader;
    w.tessellationShader = h.tessellationShader;
    w.depthClamp = h.depthClamp;
    w.depthBiasClamp = h.depthBiasClamp;
    w.samplerAnisotropy = h.samplerAnisotropy;
    w.fillModeNonSolid = h.fillModeNonSolid;
    w.independentBlend = h.independentBlend;
    w.textureCompressionBC = h.textureCompressionBC;
    w.shaderClipDistance = h.shaderClipDistance;
    w.imageCubeArray = h.imageCubeArray;
    w.shaderImageGatherExtended = h.shaderImageGatherExtended;
    w.sampleRateShading = h.sampleRateShading;
    w.dualSrcBlend = h.dualSrcBlend;
    if (std::getenv("XBE_VK_ROBUST")) w.robustBufferAccess = h.robustBufferAccess;
    if (!h.geometryShader || !h.tessellationShader || !h.textureCompressionBC || !h.shaderClipDistance)
        LOGR("vulkan: the device lacks geometry %d tessellation %d BC %d clip %d: some draws will fail", h.geometryShader,
             h.tessellationShader, h.textureCompressionBC, h.shaderClipDistance);
    w12.timelineSemaphore = VK_TRUE;
    w12.hostQueryReset = VK_TRUE;
    w13.dynamicRendering = VK_TRUE;
    w13.synchronization2 = VK_TRUE;
    w13.maintenance4 = f13.maintenance4;
    w13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;  // (DXC's discard)
    // a texel read past a texture's edge reads zeros (Direct3D's rule; the passes' Load of half size targets)
    w13.robustImageAccess = std::getenv("XBE_VK_NO_ROBUST_IMAGE") ? VK_FALSE : f13.robustImageAccess;  // (tests: off)
    if (!f12.timelineSemaphore || !f13.dynamicRendering || !f13.synchronization2) return false;
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    checkpoints = std::getenv("XBE_VK_CHECKPOINTS") != nullptr;
    if (checkpoints) exts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    VkPhysicalDeviceFaultFeaturesEXT ffault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (device_fault) {
        exts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        ffault.deviceFault = VK_TRUE;
        ffault.pNext = want.pNext;
        want.pNext = &ffault;
    }
    if (custom_border && fcb.customBorderColors && fcb.customBorderColorWithoutFormat) {
        exts.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        wcb.customBorderColors = wcb.customBorderColorWithoutFormat = VK_TRUE;
        w13.pNext = &wcb;
    } else {
        custom_border = false;
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &want;
    di.queueCreateInfoCount = nqi;
    di.pQueueCreateInfos = qi;
    di.enabledExtensionCount = uint32_t(exts.size());
    di.ppEnabledExtensionNames = exts.data();
    if (vkCreateDevice(phys, &di, nullptr, &dev) != VK_SUCCESS) {
        LOGE("vulkan: no device");
        return false;
    }
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, family, 0, &queue);
    // the depth buffers' format: D24S8 where it can be a depth buffer and sampled, else D32S8
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    g_depth_format = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    LOGR("vulkan: depth %s, copies on %s, custom border colours %d", g_depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         copy_family == ~0u ? "the main queue" : copy_family == family ? "a second graphics queue" : "a transfer queue",
         int(custom_border));
    return true;
}

bool VkDev::CreateLayouts() {
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev, &sc, nullptr, &fixed_sampler);
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(dev, &sc, nullptr, &wrap_sampler);
    sc.anisotropyEnable = VK_TRUE;
    sc.maxAnisotropy = std::min(4.0f, props.limits.maxSamplerAnisotropy);
    vkCreateSampler(dev, &sc, nullptr, &mat_sampler);
    // set 0, pushed: t0..t9, s0..s3, the fixed s4 and s5
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < kTextures; i++)
        b.push_back({kTBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    for (uint32_t i = 0; i < kSamplers; i++) b.push_back({kSBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    b.push_back({kSBinding + kSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, &fixed_sampler});
    b.push_back({kSBinding + kSamplers + 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, &wrap_sampler});
    b.push_back({kSBinding + kSamplers + 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, &mat_sampler});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = uint32_t(b.size());
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[0]) != VK_SUCCESS) return false;
    // set 1: b0..b4, dynamic offsets into the buffer they lie in
    b.clear();
    for (uint32_t i = 0; i < kCbvs; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    li.flags = 0;
    li.bindingCount = uint32_t(b.size());
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[1]) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = set_layouts;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipe_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536 * kCbvs};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 65536;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    return vkCreateDescriptorPool(dev, &dp, nullptr, &ubo_pool) == VK_SUCCESS;
}

// the null views' images, zeros; the null vertex buffer
bool VkDev::CreateNulls() {
    struct K {
        D3D12_RESOURCE_DIMENSION dim;
        uint16_t layers;
        uint32_t samples;
        VkImageViewType type;
    } const kinds[kNullKinds] = {{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 1, VK_IMAGE_VIEW_TYPE_2D},
                                 {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
                                 {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, 1, VK_IMAGE_VIEW_TYPE_CUBE},
                                 {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, 1, VK_IMAGE_VIEW_TYPE_3D},
                                 {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 4, VK_IMAGE_VIEW_TYPE_2D}};
    for (int k = 0; k < kNullKinds; k++) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = kinds[k].dim;
        td.Width = td.Height = 1;
        td.DepthOrArraySize = kinds[k].layers;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = kinds[k].samples;
        if (kinds[k].samples > 1) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!CreateResource(HeapType::Default, td, D3D12_RESOURCE_STATE_COMMON, nullptr, null_tex[k])) return false;
        auto* r = static_cast<VkRes*>(null_tex[k].Get());
        null_view[k] = r->View({VK_FORMAT_R8G8B8A8_UNORM, kinds[k].type, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kinds[k].layers, {}});
    }
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 65536;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    if (!CreateResource(HeapType::Default, bd, D3D12_RESOURCE_STATE_COMMON, nullptr, null_buf)) return false;
    OneShot([&](VkCommandBuffer c) {
        std::vector<VkImageMemoryBarrier2> bs;
        for (int k = 0; k < kNullKinds; k++) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = static_cast<VkRes*>(null_tex[k].Get())->img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            bs.push_back(b);
        }
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = uint32_t(bs.size());
        di.pImageMemoryBarriers = bs.data();
        vkCmdPipelineBarrier2(c, &di);
        VkClearColorValue zero{};
        VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        for (int k = 0; k < kNullKinds; k++) vkCmdClearColorImage(c, bs[k].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        vkCmdFillBuffer(c, static_cast<VkRes*>(null_buf.Get())->buf, 0, VK_WHOLE_SIZE, 0);
        for (auto& b : bs) {
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = kShaders;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(c, &di);
    });
    for (auto& t : null_tex) static_cast<VkRes*>(t.Get())->use = kSampled;
    return true;
}

void VkDev::OneShot(const std::function<void(VkCommandBuffer)>& f) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = family;
    vkCreateCommandPool(dev, &pi, nullptr, &pool);
    VkCommandBuffer c;
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = pool;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ci, &c);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    f(c);
    vkEndCommandBuffer(c);
    VkCommandBufferSubmitInfo cs{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cs.commandBuffer = c;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cs;
    {
        std::lock_guard l(queue_lock);
        vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }
    vkDestroyCommandPool(dev, pool, nullptr);
}

VkSemaphore VkDev::MakeSemaphore(bool timeline_kind) {
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (timeline_kind) si.pNext = &ti;
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &si, nullptr, &s);
    return s;
}

VkDev::~VkDev() {
    if (!dev) {
        if (instance) vkDestroyInstance(instance, nullptr);
        return;
    }
    vkDeviceWaitIdle(dev);
    bb.clear();
    null_buf.Reset();
    for (auto& t : null_tex) t.Reset();
    Collect(true);
    for (auto& h : heaps) h.clear();
    for (auto& [k, s] : samplers) vkDestroySampler(dev, s, nullptr);
    for (VkSemaphore s : acquire_sems) vkDestroySemaphore(dev, s, nullptr);
    for (VkSemaphore s : present_sems) vkDestroySemaphore(dev, s, nullptr);
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    for (uint32_t i = 0; i < kSlots; i++) {
        vkDestroyCommandPool(dev, pools[i], nullptr);
        for (VkDescriptorPool p : transient[i]) vkDestroyDescriptorPool(dev, p, nullptr);
    }
    vkDestroyDescriptorPool(dev, ubo_pool, nullptr);
    vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
    for (auto l : set_layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    vkDestroySampler(dev, fixed_sampler, nullptr);
    vkDestroySampler(dev, wrap_sampler, nullptr);
    vkDestroySampler(dev, mat_sampler, nullptr);
    if (cache) vkDestroyPipelineCache(dev, cache, nullptr);
    vkDestroySemaphore(dev, timeline, nullptr);
    vmaDestroyAllocator(vma);
    vkDestroyDevice(dev, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
}

// --- presentation

bool VkDev::CreateSwapChain(HWND, uint32_t w, uint32_t h, uint32_t n) {
    bb_n = n;
    vsync = app::Get().vsync;
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes.data());
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = true;
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) mailbox = true;
    }
    return MakeSwapChain(w, h);
}

bool VkDev::MakeSwapChain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(bb_n, caps.minImageCount);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc_extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{w, h};
    sc_extent.width = std::max(1u, sc_extent.width), sc_extent.height = std::max(1u, sc_extent.height);
    ci.imageExtent = sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // vsync: FIFO; else tearing as D3D12's (immediate), mailbox where there is none
    ci.presentMode = vsync ? VK_PRESENT_MODE_FIFO_KHR : immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                                                          : VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR old = swapchain;
    ci.oldSwapchain = old;
    if (vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain) != VK_SUCCESS) {
        LOGE("vulkan: no swap chain");
        return false;
    }
    std::vector<Ref<Resource>> old_bb = std::move(bb);
    bb.clear();
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(dev, swapchain, &n, images.data());
    if (n != bb_n) LOGE("vulkan: the swap chain has %u images, not %u", n, bb_n);
    LOGR("vulkan: swap chain %ux%u, %u images, present mode %d", sc_extent.width, sc_extent.height, n, int(ci.presentMode));
    for (VkImage im : images) {
        auto* r = new VkRes(*this);
        r->img = im;
        r->swapchain = true;
        r->format = VK_FORMAT_B8G8R8A8_UNORM;
        r->fmt = FormatOf(DXGI_FORMAT_B8G8R8A8_UNORM);
        r->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        r->extent = {sc_extent.width, sc_extent.height, 1};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = sc_extent.width;
        d.Height = sc_extent.height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        r->Describe(d, HeapType::Default);
        Register(r);
        bb.push_back(Ref<Resource>::Adopt(r));
    }
    // the views of the old images (made by the renderer, which a swap chain made again here does not hear of) on the new
    for (Slot& s : heaps[int(Heap::Rtv)])
        for (size_t i = 0; i < old_bb.size() && i < bb.size(); i++)
            if (s.res == old_bb[i].Get()) {
                auto* r = static_cast<VkRes*>(bb[i].Get());
                s.res = r;
                s.serial = r->serial, s.place = r->place;
                s.view = r->View({r->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, {}});
                s.extent = sc_extent;
            }
    old_bb.clear();
    while (acquire_sems.size() < n + 1) acquire_sems.push_back(MakeSemaphore(false));
    while (present_sems.size() < n) present_sems.push_back(MakeSemaphore(false));
    image = ~0u;
    return true;
}

uint32_t VkDev::CurrentBackBuffer() {
    if (image != ~0u) return image;
    for (int tries = 0; tries < 3; tries++) {
        VkSemaphore s = acquire_sems[acquire_next];
        VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, s, VK_NULL_HANDLE, &image);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            acquire_next = (acquire_next + 1) % uint32_t(acquire_sems.size());
            pending_acquire = s;
            auto* b = static_cast<VkRes*>(bb[image].Get());
            // (its contents are the presentation engine's: drawn over whole)
            b->use = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE,
                      VK_IMAGE_LAYOUT_UNDEFINED};
            return std::min(image, uint32_t(bb.size()) - 1);
        }
        LOG_FIRST(8, "vulkan: acquire %d", int(r));
        image = ~0u;
        vkDeviceWaitIdle(dev);
        MakeSwapChain(sc_extent.width, sc_extent.height);
    }
    return 0;
}

bool VkDev::ResizeSwapChain(uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(dev);
    image = ~0u;
    pending_acquire = VK_NULL_HANDLE;
    return MakeSwapChain(w, h);
}

void VkDev::Present(bool vs) {
    static uint64_t presents = 0;
    if (++presents % 120 == 0 && std::getenv("XBE_VK_TRACE")) {
        uint64_t v = 0;
        VkResult r = vkGetSemaphoreCounterValue(dev, timeline, &v);
        static uint64_t last[4] = {};
        LOGR("vulkan: present %llu image %u, completed %llu (%d) of %llu; a frame: %.0f draws, %.0f rendering instances, %.0f "
             "barrier calls, %.0f clears outside; %llu dropped",
             (unsigned long long)presents, image, (unsigned long long)v, int(r), (unsigned long long)submitted.load(),
             (list.draws - last[0]) / 120.0, (list.passes - last[1]) / 120.0, (list.barriers - last[2]) / 120.0,
             (list.clears - last[3]) / 120.0, (unsigned long long)list.dropped);
        last[0] = list.draws, last[1] = list.passes, last[2] = list.barriers, last[3] = list.clears;
        VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(vma, b);
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(vma, &mp);
        for (uint32_t h = 0; mp && h < mp->memoryHeapCount; h++)
            LOGR("vulkan: heap %u%s: %llu allocations, %.0f MB used of %.0f MB budget (%.0f MB heap)", h,
                 mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (GPU)" : "", (unsigned long long)b[h].statistics.allocationCount,
                 b[h].usage / 1048576.0, b[h].budget / 1048576.0, mp->memoryHeaps[h].size / 1048576.0);
    }
    static const uint64_t dump_at = [] {
        const char* e = std::getenv("XBE_VK_DUMP_FRAME");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    list.dump = dump_at && presents == dump_at;
    if (image == ~0u) return;
    uint32_t i = image;
    image = ~0u;
    // the frame's last submit before: its signal waited for by the presentation
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = present_sems[i];
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &present_sems[i];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &i;
    VkResult r;
    {
        std::lock_guard l(queue_lock);
        vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
        r = vkQueuePresentKHR(queue, &pi);
    }
    if (r != VK_SUCCESS) LOG_FIRST(8, "vulkan: present %d", int(r));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || vs != vsync) {
        vsync = vs;
        vkDeviceWaitIdle(dev);
        MakeSwapChain(sc_extent.width, sc_extent.height);
    }
}

// --- resources and views

void VkDev::CreateHeaps(const HeapSizes& s) {
    heaps[int(Heap::Rtv)].assign(s.rtv, Slot{});
    heaps[int(Heap::Dsv)].assign(s.dsv, Slot{});
    heaps[int(Heap::Srv)].assign(s.srv, Slot{});
    heaps[int(Heap::Sampler)].assign(s.sampler, Slot{});
}

bool VkDev::CreateResource(HeapType heap, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                           Ref<Resource>& out) {
    auto* r = new VkRes(*this);
    r->Describe(d, heap);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        // (an upload buffer's constant buffers read up to 64 KB past where they start)
        bi.size = d.Width + (heap == HeapType::Upload ? kCbvRange : 0);
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (heap == HeapType::Upload)
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (heap == HeapType::Readback)
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, r->readback = true;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(vma, &bi, &ai, &r->buf, &r->alloc, &info) != VK_SUCCESS) {
            delete r;
            out.Reset();
            return false;
        }
        r->map = static_cast<uint8_t*>(info.pMappedData);
        Register(r);
        {
            std::lock_guard l(id_lock);
            if (!free_ids.empty()) r->id = free_ids.back(), free_ids.pop_back();
            else r->id = next_id++;
        }
        buffers[r->id].store(r, std::memory_order_release);
        out = Ref<Resource>::Adopt(r);
        return true;
    }
    Fmt f = FormatOf(d.Format);
    if (!f.vk) {
        LOGE("vulkan: no format for DXGI format %u", unsigned(d.Format));
        delete r;
        out.Reset();
        return false;
    }
    bool ds = d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (ds && d.Format == DXGI_FORMAT_R32_TYPELESS) f = FormatOf(DXGI_FORMAT_D32_FLOAT);
    if (ds && d.Format == DXGI_FORMAT_R16_TYPELESS) f = FormatOf(DXGI_FORMAT_D16_UNORM);
    r->fmt = f;
    r->format = f.vk;
    r->aspect = f.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    r->is3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    r->mips = std::max<uint32_t>(1, d.MipLevels);
    r->layers = r->is3d ? 1 : d.DepthOrArraySize;
    r->samples = std::max<uint32_t>(1, d.SampleDesc.Count);
    r->extent = {uint32_t(d.Width), d.Height, r->is3d ? d.DepthOrArraySize : 1u};
    r->cube = !r->is3d && r->layers % 6 == 0 && d.Width == d.Height && r->samples == 1;
    r->mutable_format = IsTypeless(d.Format) && !f.depth;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = (r->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (r->mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    ii.imageType = r->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = f.vk;
    ii.extent = r->extent;
    ii.mipLevels = r->mips;
    ii.arrayLayers = r->layers;
    ii.samples = VkSampleCountFlagBits(r->samples);
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ds) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    r->zero = d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // targets alone in their memory (the drivers' advice for them)
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(vma, &ii, &ai, &r->img, &r->alloc, nullptr) != VK_SUCCESS) {
        LOGE("vulkan: image %llux%u format %u failed", (unsigned long long)d.Width, d.Height, unsigned(d.Format));
        delete r;
        out.Reset();
        return false;
    }
    {
        VkMemoryPropertyFlags mf = 0;
        vmaGetAllocationMemoryProperties(vma, r->alloc, &mf);
        if (!(mf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            LOG_FIRST(16, "vulkan: image %llux%u format %u not in the GPU's memory (flags %X)", (unsigned long long)d.Width, d.Height,
                      unsigned(d.Format), unsigned(mf));
    }
    Register(r);
    out = Ref<Resource>::Adopt(r);
    return true;
}

void VkDev::CreateRtv(Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d, View at) {
    Slot& s = heaps[int(Heap::Rtv)][at.index];
    auto* r = static_cast<VkRes*>(res);
    s = Slot{};
    if (!r) return;
    uint32_t mip = d && d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D ? d->Texture2D.MipSlice : 0;
    VkFormat f = d && d->Format ? FormatOf(d->Format).vk : r->format;
    s.res = r;
    s.serial = r->serial, s.place = r->place;
    s.view = r->View({f, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1, {}});
    s.format = f;
    s.extent = {std::max(1u, r->extent.width >> mip), std::max(1u, r->extent.height >> mip)};
    s.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
}

void VkDev::CreateDsv(Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC*, View at) {
    Slot& s = heaps[int(Heap::Dsv)][at.index];
    auto* r = static_cast<VkRes*>(res);
    s = Slot{};
    if (!r) return;
    s.res = r;
    s.serial = r->serial, s.place = r->place;
    s.view = r->View({r->format, VK_IMAGE_VIEW_TYPE_2D, r->aspect, 0, 1, 0, 1, {}});
    s.format = r->format;
    s.extent = {r->extent.width, r->extent.height};
    s.aspect = r->aspect;
}

VkComponentSwizzle Swizzle(UINT mapping, int i) {
    switch (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, mapping)) {
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0: return VK_COMPONENT_SWIZZLE_R;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1: return VK_COMPONENT_SWIZZLE_G;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2: return VK_COMPONENT_SWIZZLE_B;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3: return VK_COMPONENT_SWIZZLE_A;
        case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0: return VK_COMPONENT_SWIZZLE_ZERO;
        default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

void VkDev::CreateSrv(Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, View at) {
    Slot& s = heaps[int(Heap::Srv)][at.index];
    s = Slot{};
    D3D12_SRV_DIMENSION dim = d ? d->ViewDimension : D3D12_SRV_DIMENSION_TEXTURE2D;
    s.null_kind = dim == D3D12_SRV_DIMENSION_TEXTURECUBE ? kNullCube
                  : dim == D3D12_SRV_DIMENSION_TEXTURE3D ? kNull3D
                  : dim == D3D12_SRV_DIMENSION_TEXTURE2DARRAY ? kNull2DArray
                  : dim == D3D12_SRV_DIMENSION_TEXTURE2DMS ? kNull2DMS
                                                           : kNull2D;
    auto* r = static_cast<VkRes*>(res);
    if (!r || !r->img) return;
    VkRes::ViewKey k{};
    k.format = d && d->Format ? FormatOf(d->Format).vk : r->format;
    k.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (r->fmt.depth) {
        // a depth buffer read: its depth (its stencil for X24_TYPELESS_G8_UINT), in its own format
        k.format = r->format;
        k.aspect = d && d->Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (k.format != r->format && !r->mutable_format) {
        LOG_FIRST(8, "vulkan: a view of format %d of an image of format %d", int(k.format), int(r->format));
        k.format = r->format;
    }
    UINT mapping = d ? d->Shader4ComponentMapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (d && d->Format == DXGI_FORMAT_B8G8R8X8_UNORM) mapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, 5);
    if (d && d->Format == DXGI_FORMAT_A8_UNORM) mapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(4, 4, 4, 0);
    k.swizzle = {Swizzle(mapping, 0), Swizzle(mapping, 1), Swizzle(mapping, 2), Swizzle(mapping, 3)};
    auto mips = [&](UINT most, UINT n) {
        k.mip = std::min<uint32_t>(most, r->mips - 1);
        k.mips = n == UINT(-1) || n == 0 ? r->mips - k.mip : std::min<uint32_t>(n, r->mips - k.mip);
    };
    k.layers = 1;
    switch (dim) {
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            k.type = r->cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            k.layers = r->cube ? 6 : 1;
            mips(d->TextureCube.MostDetailedMip, d->TextureCube.MipLevels);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            k.type = VK_IMAGE_VIEW_TYPE_3D;
            mips(d->Texture3D.MostDetailedMip, d->Texture3D.MipLevels);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            k.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            k.layer = std::min<uint32_t>(d->Texture2DArray.FirstArraySlice, r->layers - 1);
            k.layers = std::min<uint32_t>(std::max<uint32_t>(d->Texture2DArray.ArraySize, 1), r->layers - k.layer);
            mips(d->Texture2DArray.MostDetailedMip, d->Texture2DArray.MipLevels);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            k.type = VK_IMAGE_VIEW_TYPE_2D;
            k.mip = 0, k.mips = 1;
            break;
        default:
            k.type = VK_IMAGE_VIEW_TYPE_2D;
            mips(d ? d->Texture2D.MostDetailedMip : 0, d ? d->Texture2D.MipLevels : UINT(-1));
            break;
    }
    static const bool trace = std::getenv("XBE_VK_TRACE") != nullptr;
    if (trace && !r->fmt.depth && (k.mips < r->mips || (r->mips == 1 && r->extent.width >= 256 && !(r->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))))
        LOG_FIRST(60, "vulkan: srv of %ux%u format %d, %u mips: view mips %u..+%u (asked %d)", r->extent.width, r->extent.height,
                  int(r->format), r->mips, k.mip, k.mips, d ? int(d->Texture2D.MipLevels) : -1);
    s.res = r;
    s.serial = r->serial, s.place = r->place;
    s.view = r->View(k);
    s.aspect = k.aspect;
}

void VkDev::CreateSampler(const D3D12_SAMPLER_DESC& d, View at) {
    uint64_t key = 1469598103934665603ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    for (size_t i = 0; i < sizeof(d); i++) key = (key ^ p[i]) * 1099511628211ull;
    VkSampler& smp = samplers[key];
    if (!smp) {
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        bool aniso = D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter);
        sc.magFilter = aniso || D3D12_DECODE_MAG_FILTER(d.Filter) == D3D12_FILTER_TYPE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.minFilter = aniso || D3D12_DECODE_MIN_FILTER(d.Filter) == D3D12_FILTER_TYPE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.mipmapMode = aniso || D3D12_DECODE_MIP_FILTER(d.Filter) == D3D12_FILTER_TYPE_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU = AddressMode(d.AddressU);
        sc.addressModeV = AddressMode(d.AddressV);
        sc.addressModeW = AddressMode(d.AddressW);
        sc.mipLodBias = d.MipLODBias;
        if (aniso && d.MaxAnisotropy > 1) {
            sc.anisotropyEnable = VK_TRUE;
            sc.maxAnisotropy = std::min(float(d.MaxAnisotropy), props.limits.maxSamplerAnisotropy);
        }
        if (D3D12_DECODE_IS_COMPARISON_FILTER(d.Filter)) sc.compareEnable = VK_TRUE, sc.compareOp = CompareOp(d.ComparisonFunc);
        sc.minLod = d.MinLOD;
        sc.maxLod = d.MaxLOD >= 1e30f ? VK_LOD_CLAMP_NONE : d.MaxLOD;
        VkSamplerCustomBorderColorCreateInfoEXT cb{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
        const float* bc = d.BorderColor;
        bool used = sc.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || sc.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                    sc.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        if (!used || (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 0)) sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 1) sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1) sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (custom_border) {
            sc.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            std::memcpy(cb.customBorderColor.float32, bc, 16);
            sc.pNext = &cb;
        } else {
            sc.borderColor = bc[0] + bc[1] + bc[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                             : bc[3] > 0.5f             ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                        : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        }
        if (vkCreateSampler(dev, &sc, nullptr, &smp) != VK_SUCCESS) smp = fixed_sampler;
    }
    heaps[int(Heap::Sampler)][at.index].sampler = smp;
}

void VkDev::Footprints(const D3D12_RESOURCE_DESC& d, uint32_t first, uint32_t n, uint64_t base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,
                       UINT* rows, UINT64* row_bytes, UINT64* total) {
    Fmt f = FormatOf(d.Format);
    uint32_t mips = std::max<uint32_t>(1, d.MipLevels);
    bool is3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (fp) fp[0] = {base, {DXGI_FORMAT_UNKNOWN, UINT(d.Width), 1, 1, UINT(d.Width)}};
        if (rows) rows[0] = 1;
        if (row_bytes) row_bytes[0] = d.Width;
        if (total) *total = d.Width;
        return;
    }
    uint64_t off = base, end = base;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sub = first + i, mip = sub % mips;
        uint32_t w = std::max(1u, uint32_t(d.Width) >> mip), h = std::max(1u, d.Height >> mip);
        uint32_t dp = is3d ? std::max(1u, uint32_t(d.DepthOrArraySize) >> mip) : 1;
        uint32_t bw = (w + f.block - 1) / f.block, bh = (h + f.block - 1) / f.block;
        uint64_t rb = uint64_t(bw) * f.bytes;
        uint32_t pitch = uint32_t((rb + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
        off = (off + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        if (fp) fp[i] = {off, {d.Format, bw * f.block, bh * f.block, dp, pitch}};
        if (rows) rows[i] = bh;
        if (row_bytes) row_bytes[i] = rb;
        uint64_t size = uint64_t(pitch) * (uint64_t(bh) * dp - 1) + rb;
        end = off + size;
        off = end;
    }
    if (total) *total = end - base;
}

bool VkDev::SupportsSamples(DXGI_FORMAT f, uint32_t n) {
    Fmt x = FormatOf(f);
    VkImageFormatProperties ip{};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | (x.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (vkGetPhysicalDeviceImageFormatProperties(phys, x.vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ip) != VK_SUCCESS) return false;
    return (ip.sampleCounts & n) != 0;
}

VkDescriptorSet VkDev::MixedSet(VkRes* const bufs[kCbvs]) {
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < kCbvs; i++) key = (key ^ bufs[i]->serial) * 1099511628211ull;
    VkDescriptorSet& set = mixed[slot][key];
    if (set) return set;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layouts[1];
    for (int attempt = 0; attempt < 2 && !set; attempt++) {
        if (attempt || transient[slot].empty()) {
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024 * kCbvs};
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dp.maxSets = 1024;
            dp.poolSizeCount = 1;
            dp.pPoolSizes = &ps;
            VkDescriptorPool p;
            if (vkCreateDescriptorPool(dev, &dp, nullptr, &p) != VK_SUCCESS) break;
            transient[slot].push_back(p);
        }
        ai.descriptorPool = transient[slot].back();
        if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) set = VK_NULL_HANDLE;
    }
    if (!set) {
        LOG_FIRST(4, "vulkan: no constant buffer set");
        return VK_NULL_HANDLE;
    }
    VkDescriptorBufferInfo bi[kCbvs];
    VkWriteDescriptorSet ws[kCbvs];
    for (uint32_t i = 0; i < kCbvs; i++) {
        bi[i] = {bufs[i]->buf, 0, CbvRange(bufs[i])};
        ws[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[i].dstSet = set;
        ws[i].dstBinding = i;
        ws[i].descriptorCount = 1;
        ws[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ws[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(dev, kCbvs, ws, 0, nullptr);
    return set;
}

VkDescriptorSet VkDev::UboSet(VkRes* r) {
    if (r->ubo) return r->ubo;
    std::lock_guard l(ubo_lock);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = ubo_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layouts[1];
    if (vkAllocateDescriptorSets(dev, &ai, &r->ubo) != VK_SUCCESS) {
        LOGE("vulkan: no constant buffer set");
        return VK_NULL_HANDLE;
    }
    VkDescriptorBufferInfo bi{r->buf, 0, CbvRange(r)};
    VkWriteDescriptorSet ws[kCbvs];
    for (uint32_t i = 0; i < kCbvs; i++) {
        ws[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[i].dstSet = r->ubo;
        ws[i].dstBinding = i;
        ws[i].descriptorCount = 1;
        ws[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ws[i].pBufferInfo = &bi;
    }
    vkUpdateDescriptorSets(dev, kCbvs, ws, 0, nullptr);
    return r->ubo;
}

// --- shaders and pipelines

Ref<Blob> VkDev::Compile(const std::string& src, const char* entry, const char* profile, std::string& error) {
    static thread_local ComPtr<IDxcCompiler3> compiler;
    if (!compiler) {
        DxcCreate create = DxcLoader();
        if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
            error = "no DXC";
            return nullptr;
        }
    }
    static const std::vector<std::wstring> base = DxcArgs();
    std::wstring e(entry, entry + std::strlen(entry));
    // vs_5_0 -> vs_6_0
    std::wstring p(profile, profile + std::strlen(profile));
    if (p.size() >= 6) p = p.substr(0, 3) + L"6_0";
    std::vector<LPCWSTR> args;
    for (const auto& a : base) args.push_back(a.c_str());
    args.push_back(L"-E");
    args.push_back(e.c_str());
    args.push_back(L"-T");
    args.push_back(p.c_str());
    DxcBuffer buf{src.data(), src.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> res;
    if (FAILED(compiler->Compile(&buf, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&res)))) {
        error = "DXC failed";
        return nullptr;
    }
    HRESULT status = E_FAIL;
    res->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> err;
        res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        error = err && err->GetStringLength() ? err->GetStringPointer() : "?";
        return nullptr;
    }
    ComPtr<IDxcBlob> code;
    res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
    if (!code) {
        error = "no code";
        return nullptr;
    }
    return Ref<Blob>::Adopt(new Blob(code->GetBufferPointer(), code->GetBufferSize()));
}

Ref<Pipeline> VkDev::CreatePipeline(const PipelineDesc& pd) {
    if (!pd.VS) return nullptr;
    // the stages in order, each one's inputs where the one before puts its outputs
    struct Stage {
        const Blob* blob;
        VkShaderStageFlagBits stage;
    };
    auto* pipe = new VkPipe(*this);
    Ref<Pipeline> keep = Ref<Pipeline>::Adopt(pipe);
    std::vector<Stage> order = {{pd.VS, VK_SHADER_STAGE_VERTEX_BIT}};
    if (pd.HS && pd.DS) order.push_back({pd.HS, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT}), order.push_back({pd.DS, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT});
    if (pd.GS) order.push_back({pd.GS, VK_SHADER_STAGE_GEOMETRY_BIT});
    pipe->gs = pd.GS != nullptr, pipe->tess = pd.HS && pd.DS;
    if (pd.PS) order.push_back({pd.PS, VK_SHADER_STAGE_FRAGMENT_BIT});
    std::vector<Spirv> code;
    for (const Stage& s : order) code.emplace_back(s.blob);
    for (size_t i = 0; i < code.size(); i++) {
        if (i > 0) {
            std::string missing;
            if (!code[i].MatchInputs(code[i - 1].Interface(3), missing)) {
                LOG_FIRST(16, "vulkan: a pipeline's stage %d reads %s, which the stage before does not write", int(order[i].stage),
                          missing.c_str());
                return nullptr;
            }
        }
        if (!code[i].Bindings(pipe->textures, pipe->samplers, pipe->cbvs)) return nullptr;
    }
    std::vector<VkShaderModule> modules;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (size_t i = 0; i < code.size(); i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code[i].w.size() * 4;
        mi.pCode = code[i].w.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &mi, nullptr, &m) != VK_SUCCESS) {
            for (auto x : modules) vkDestroyShaderModule(dev, x, nullptr);
            return nullptr;
        }
        modules.push_back(m);
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = order[i].stage;
        si.module = m;
        si.pName = code[i].entry.c_str();
        stages.push_back(si);
    }
    // vertex input: each element at the location the vertex shader gives its semantic
    auto inputs = code[0].Interface(1);
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    uint32_t offsets[16] = {};
    for (UINT i = 0; i < pd.InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = pd.InputLayout.pInputElementDescs[i];
        std::string sem = e.SemanticName;
        for (char& c : sem) c = char(std::toupper(static_cast<unsigned char>(c)));
        sem += std::to_string(e.SemanticIndex);
        Fmt f = FormatOf(e.Format);
        uint32_t off = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? offsets[e.InputSlot] : e.AlignedByteOffset;
        offsets[e.InputSlot] = off + f.bytes;
        auto it = inputs.find(sem);
        if (it == inputs.end()) continue;
        attrs.push_back({it->second, e.InputSlot, f.vk, off});
        bool have = false;
        for (auto& b : binds) have = have || b.binding == e.InputSlot;
        if (!have)
            binds.push_back({e.InputSlot, 0, e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                                             : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (pd.PrimitiveTopologyType) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH: ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
        default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = 3;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    const D3D12_RASTERIZER_DESC& rd = pd.RasterizerState;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = !rd.DepthClipEnable;
    rs.polygonMode = rd.FillMode == D3D12_FILL_MODE_WIREFRAME ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = rd.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT : rd.CullMode == D3D12_CULL_MODE_FRONT ? VK_CULL_MODE_FRONT_BIT
                                                                                                                       : VK_CULL_MODE_NONE;
    // (the viewport flipped: the window's clockwise is the framebuffer's as Direct3D sees it)
    rs.frontFace = rd.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = rd.DepthBias != 0 || rd.SlopeScaledDepthBias != 0;
    rs.depthBiasConstantFactor = float(rd.DepthBias);
    rs.depthBiasClamp = rd.DepthBiasClamp;
    rs.depthBiasSlopeFactor = rd.SlopeScaledDepthBias;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max<UINT>(1, pd.SampleDesc.Count));
    VkSampleMask mask = pd.SampleMask;
    ms.pSampleMask = &mask;
    ms.alphaToCoverageEnable = pd.BlendState.AlphaToCoverageEnable;
    const D3D12_DEPTH_STENCIL_DESC& dd = pd.DepthStencilState;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = dd.DepthEnable;
    ds.depthWriteEnable = dd.DepthEnable && dd.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    ds.depthCompareOp = dd.DepthEnable ? CompareOp(dd.DepthFunc) : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = dd.StencilEnable;
    auto face = [&](const D3D12_DEPTH_STENCILOP_DESC& f) {
        VkStencilOpState s{};
        s.failOp = StencilOp(f.StencilFailOp);
        s.passOp = StencilOp(f.StencilPassOp);
        s.depthFailOp = StencilOp(f.StencilDepthFailOp);
        s.compareOp = CompareOp(f.StencilFunc);
        s.compareMask = dd.StencilReadMask;
        s.writeMask = dd.StencilWriteMask;
        return s;
    };
    ds.front = face(dd.FrontFace);
    ds.back = face(dd.BackFace);
    VkPipelineColorBlendAttachmentState cb[8]{};
    for (UINT i = 0; i < pd.NumRenderTargets; i++) {
        const D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[pd.BlendState.IndependentBlendEnable ? i : 0];
        cb[i].blendEnable = b.BlendEnable;
        cb[i].srcColorBlendFactor = BlendFactor(b.SrcBlend);
        cb[i].dstColorBlendFactor = BlendFactor(b.DestBlend);
        cb[i].colorBlendOp = BlendOp(b.BlendOp);
        cb[i].srcAlphaBlendFactor = BlendFactor(b.SrcBlendAlpha);
        cb[i].dstAlphaBlendFactor = BlendFactor(b.DestBlendAlpha);
        cb[i].alphaBlendOp = BlendOp(b.BlendOpAlpha);
        cb[i].colorWriteMask = b.RenderTargetWriteMask;
    }
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = pd.NumRenderTargets;
    bs.pAttachments = cb;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                  VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkFormat colors[8];
    for (UINT i = 0; i < pd.NumRenderTargets; i++) colors[i] = FormatOf(pd.RTVFormats[i]).vk;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = pd.NumRenderTargets;
    ri.pColorAttachmentFormats = colors;
    if (pd.DSVFormat) {
        Fmt f = FormatOf(pd.DSVFormat);
        ri.depthAttachmentFormat = f.vk;
        if (f.stencil) ri.stencilAttachmentFormat = f.vk;
    }
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = uint32_t(stages.size());
    gi.pStages = stages.data();
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pTessellationState = pd.HS ? &ts : nullptr;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &bs;
    gi.pDynamicState = &dy;
    gi.layout = pipe_layout;
    static std::atomic<uint32_t> numbers{0};
    pipe->number = ++numbers;
    static const bool trace = std::getenv("XBE_VK_TRACE") != nullptr;
    if (checkpoints || trace)
        for (auto& c : code) pipe->code.push_back(c.w);
    VkResult r = vkCreateGraphicsPipelines(dev, cache, 1, &gi, nullptr, &pipe->p);
    for (auto m : modules) vkDestroyShaderModule(dev, m, nullptr);
    if (r != VK_SUCCESS) {
        LOG_FIRST(16, "vulkan: pipeline creation failed (%d)", int(r));
        return nullptr;
    }
    return keep;
}

bool VkPipeCache::Load(const wchar_t*, const PipelineDesc& pd, Ref<Pipeline>& out) {
    out = d.CreatePipeline(pd);
    return bool(out);
}

std::vector<uint8_t> VkPipeCache::Serialize() {
    size_t n = 0;
    if (vkGetPipelineCacheData(d.dev, d.cache, &n, nullptr) != VK_SUCCESS || !n) return {};
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(d.dev, d.cache, &n, data.data()) != VK_SUCCESS) return {};
    data.resize(n);
    return data;
}

std::unique_ptr<PipelineCache> VkDev::OpenPipelineCache(const std::vector<uint8_t>& data) {
    if (cache) vkDestroyPipelineCache(dev, cache, nullptr);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev, &ci, nullptr, &cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(dev, &ci, nullptr, &cache) != VK_SUCCESS) return nullptr;
    }
    return std::make_unique<VkPipeCache>(*this);
}

Ref<QueryPool> VkDev::CreateTimestamps(uint32_t n) {
    auto* q = new VkQueries(*this);
    Ref<QueryPool> keep = Ref<QueryPool>::Adopt(q);
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = n;
    if (vkCreateQueryPool(dev, &ci, nullptr, &q->pool) != VK_SUCCESS) return nullptr;
    vkResetQueryPool(dev, q->pool, 0, n);
    return keep;
}

// --- the main queue

void VkDev::Submit(uint64_t value) {
    // the readbacks' writes visible to the CPU once the fence passes
    list.EndRendering();
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    list.FlushBarriers();
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(list.cb, &di);
    list.End();
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = list.cb;
    VkSemaphoreSubmitInfo waits[2];
    // (XBE_VK_SPLIT's command buffers: the slot's own again from its next reset)
    uint32_t nw = 0;
    if (pending_acquire) {
        waits[nw] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[nw].semaphore = pending_acquire;
        waits[nw].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        nw++;
        pending_acquire = VK_NULL_HANDLE;
    }
    if (uploader_wait && uploader_value) {
        waits[nw] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[nw].semaphore = uploader_wait->timeline;
        waits[nw].value = uploader_value;
        waits[nw].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nw++;
        uploader_value = 0;
    }
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = timeline;
    sig.value = value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = nw;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    std::lock_guard l(queue_lock);
    submitted = std::max(submitted.load(), value);
    if (VkResult r = vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE); r != VK_SUCCESS) {
        LOG_FIRST(4, "vulkan: submit failed (%d)", int(r));
        if (r == VK_ERROR_DEVICE_LOST) Lost();
    }
}

void VkDev::Reset(uint32_t s) {
    slot = s % kSlots;
    vkResetCommandPool(dev, pools[slot], 0);
    for (VkDescriptorPool p : transient[slot]) vkResetDescriptorPool(dev, p, 0);
    mixed[slot].clear();
    for (auto& [pool, i] : queries_used[slot]) vkResetQueryPool(dev, pool, i, 1);
    queries_used[slot].clear();
    Collect(false);
    list.Begin(cbs[slot]);
}

// the device lost: the last commands the GPU reached, their shaders written to logs/fault_<n>_<stage>.spv
void VkDev::Report() {
  for (VkQueue q : {queue, upload_queue}) {
    if (!q) continue;
    uint32_t n = 0, n1 = 0;
    vkGetQueueCheckpointData2NV(q, &n, nullptr);
    vkGetQueueCheckpointDataNV(q, &n1, nullptr);
    LOGE("vulkan: fault: %s queue, %u checkpoints (%u)", q == queue ? "main" : "upload", n, n1);
    if (!n && n1) {
        std::vector<VkCheckpointDataNV> c1(n1, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        vkGetQueueCheckpointDataNV(q, &n1, c1.data());
        for (auto& c : c1) {
            uint32_t m = uint32_t(reinterpret_cast<uintptr_t>(c.pCheckpointMarker)) & 0xFFFF;
            LOGE("vulkan: fault: checkpoint %u stage %x: %s pipeline %u", m, unsigned(c.stage), marks[m].what ? marks[m].what : "?",
                 marks[m].pipe ? marks[m].pipe->number : 0);
        }
    }
    std::vector<VkCheckpointData2NV> cp(n, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV});
    vkGetQueueCheckpointData2NV(q, &n, cp.data());
    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = uint32_t(reinterpret_cast<uintptr_t>(cp[i].pCheckpointMarker)) & 0xFFFF;
        const Mark& k = marks[m];
        LOGE("vulkan: fault: checkpoint %u at stage %llx: %s pipeline %u (newest %u)", m, (unsigned long long)cp[i].stage,
             k.what ? k.what : "?", k.pipe ? k.pipe->number : 0, (mark_next - 1) & 0xFFFF);
        if (!k.pipe) continue;
        for (size_t j = 0; j < k.pipe->code.size(); j++) {
            auto path = app::ExeDir() / "logs" / ("fault_" + std::to_string(m) + "_" + std::to_string(j) + ".spv");
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                std::fwrite(k.pipe->code[j].data(), 4, k.pipe->code[j].size(), f);
                std::fclose(f);
            }
        }
    }
  }
}

std::unique_ptr<Uploader> VkDev::CreateUploader() {
    auto u = std::make_unique<VkUploader>(*this);
    if (copy_family == ~0u) {
        u->family = family;
        u->queue = queue;
        u->queue_lock = &queue_lock;
    } else {
        u->family = copy_family;
        vkGetDeviceQueue(dev, copy_family, copy_index, &u->queue);
        upload_queue = u->queue;
    }
    for (int i = 0; i < 2; i++) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = u->family;
        vkCreateCommandPool(dev, &pi, nullptr, &u->pool[i]);
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = u->pool[i];
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ci, &u->cbs[i]);
    }
    u->timeline = MakeSemaphore(true);
    return u;
}

}  // namespace

std::unique_ptr<Device> CreateVulkan(bool debug) {
    auto d = std::make_unique<VkDev>();
    if (!d->Init(debug)) return nullptr;
    return d;
}

}  // namespace gpu::rhi
