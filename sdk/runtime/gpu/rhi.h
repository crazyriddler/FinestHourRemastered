#pragma once
// The renderer's graphics API (gpu/renderer.cpp): what it makes and records, done by Direct3D 12 (rhi_d3d12.cpp) or by
// Vulkan (rhi_vulkan.cpp). Resources, views and pipelines are described with Direct3D 12's plain structures (d3d12.h's
// descriptions, DXGI formats, resource states, viewports): each backend makes its own objects from them, natively.
//
// The model is the renderer's: views written into numbered places of a few tables (render targets, depth buffers,
// shader resources, samplers), the shaders reading a range of them; constant buffers by their place in a buffer;
// a resource's state changed by the renderer before each new use. Two layouts of the shaders' arguments (Layout).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gpu::rhi {

// A buffer's place for the shaders and the input assembler: its address on the GPU (D3D12), or its number and the
// offset in it (Vulkan); either way an offset added to it is a place further in the same buffer
using GpuVa = uint64_t;

// counted references, as COM's
class Object {
public:
    virtual ~Object() = default;
    void AddRef() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void Release() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }

private:
    std::atomic<uint32_t> refs_{1};
};

// a reference to an object (ComPtr's subset): a raw pointer assigned to it is counted once more, Adopt takes a new one
template <class T>
class Ref {
public:
    Ref() = default;
    Ref(std::nullptr_t) {}
    Ref(T* p) : p_(p) {
        if (p_) p_->AddRef();
    }
    Ref(const Ref& o) : Ref(o.p_) {}
    Ref(Ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    template <class U>
    Ref(const Ref<U>& o) : Ref(static_cast<T*>(o.Get())) {}
    ~Ref() { Reset(); }
    Ref& operator=(const Ref& o) {
        Ref(o).Swap(*this);
        return *this;
    }
    Ref& operator=(Ref&& o) noexcept {
        Ref(std::move(o)).Swap(*this);
        return *this;
    }
    Ref& operator=(T* p) {
        Ref(p).Swap(*this);
        return *this;
    }
    Ref& operator=(std::nullptr_t) {
        Reset();
        return *this;
    }
    static Ref Adopt(T* p) {
        Ref r;
        r.p_ = p;
        return r;
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const Ref& o) const { return p_ == o.p_; }
    bool operator==(std::nullptr_t) const { return p_ == nullptr; }
    void Reset() {
        if (T* p = std::exchange(p_, nullptr)) p->Release();
    }
    void Swap(Ref& o) { std::swap(p_, o.p_); }

private:
    T* p_ = nullptr;
};

enum class HeapType : uint8_t { Default, Upload, Readback };

class Resource : public Object {
public:
    const D3D12_RESOURCE_DESC& GetDesc() const { return desc_; }
    HeapType Heap() const { return heap_; }
    // a buffer's place (GpuVa)
    virtual GpuVa Va() const = 0;
    // an upload or readback buffer's bytes (kept mapped); null for the others. A readback buffer's are the GPU's
    // once its copy is done (the backends make them visible to the CPU)
    virtual uint8_t* Map() = 0;
    virtual void SetName(const wchar_t*) {}

protected:
    D3D12_RESOURCE_DESC desc_{};
    HeapType heap_ = HeapType::Default;
};

// compiled shader code (DXBC for D3D12, SPIR-V for Vulkan)
class Blob : public Object {
public:
    Blob(const void* p, size_t n) : data_(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n) {}
    const void* GetBufferPointer() const { return data_.data(); }
    size_t GetBufferSize() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
};

class Pipeline : public Object {};

// timestamps the GPU writes as it passes them
class QueryPool : public Object {};

// The shaders' arguments, as numbered parameters (D3D12's root signatures):
//   Main:   0 b0, 1 b1 (constant buffers), 2 t0..t6 (shader resources), 3 s0..s3 (samplers), 4 t7..t9, 5 b2;
//           s4 is a fixed sampler (linear, clamped)
//   Motion: 0..4 b0..b4, 5 t0
enum class Layout : uint8_t { Main, Motion };

// the tables of views: render targets, depth buffers, shader resources and samplers. A table's place for the
// shaders (D3D12's GPU descriptor handle) and a view's place (its CPU handle) are both a table and an index
enum class Heap : uint8_t { Rtv, Dsv, Srv, Sampler };
struct View {
    Heap heap = Heap::Rtv;
    uint32_t index = 0;
    bool operator==(const View& o) const { return heap == o.heap && index == o.index; }
};
using Table = View;

struct HeapSizes {
    uint32_t rtv = 0, dsv = 0, srv = 0, sampler = 0;
};

// a pipeline's description, D3D12's graphics pipeline state but for its root signature (layout) and shaders (blobs)
struct PipelineDesc {
    Layout layout = Layout::Main;
    Blob* VS = nullptr;
    Blob* PS = nullptr;
    Blob* GS = nullptr;
    Blob* HS = nullptr;
    Blob* DS = nullptr;
    D3D12_BLEND_DESC BlendState{};
    UINT SampleMask = 0;
    D3D12_RASTERIZER_DESC RasterizerState{};
    D3D12_DEPTH_STENCIL_DESC DepthStencilState{};
    D3D12_INPUT_LAYOUT_DESC InputLayout{};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    UINT NumRenderTargets = 0;
    DXGI_FORMAT RTVFormats[8]{};
    DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC SampleDesc{};
};

// a copy's source or destination: a subresource of a texture, or a footprint in a buffer (D3D12's)
struct CopyLocation {
    Resource* pResource = nullptr;
    D3D12_TEXTURE_COPY_TYPE Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    union {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
        UINT SubresourceIndex;
    };
    CopyLocation() : PlacedFootprint{} {}
    CopyLocation(Resource* r, D3D12_TEXTURE_COPY_TYPE t) : pResource(r), Type(t), PlacedFootprint{} {}
};

// The commands. A list records them in order; the backend sets what a draw needs (its targets, its arguments) when
// it draws. A resource is in the state the last Barrier left it in.
class List {
public:
    virtual ~List() = default;
    virtual void SetLayout(Layout l) = 0;  // its parameters set again after
    virtual void SetPipeline(Pipeline* p) = 0;
    virtual void SetCbv(uint32_t param, GpuVa va) = 0;
    virtual void SetTable(uint32_t param, Table t) = 0;
    virtual void SetViewport(const D3D12_VIEWPORT& v) = 0;
    virtual void SetScissor(const D3D12_RECT& r) = 0;
    virtual void SetBlendFactor(const float f[4]) = 0;
    virtual void SetStencilRef(uint32_t r) = 0;
    virtual void SetTopology(D3D_PRIMITIVE_TOPOLOGY t) = 0;
    virtual void SetVertexBuffers(uint32_t start, uint32_t n, const D3D12_VERTEX_BUFFER_VIEW* v) = 0;
    virtual void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) = 0;
    // n colour targets (rtv, may be null for none) and a depth buffer (dsv, null for none)
    virtual void SetTargets(uint32_t n, const View* rtv, const View* dsv) = 0;
    virtual void ClearRtv(View v, const float color[4], uint32_t n, const D3D12_RECT* rects) = 0;
    virtual void ClearDsv(View v, D3D12_CLEAR_FLAGS flags, float z, uint8_t stencil, uint32_t n, const D3D12_RECT* rects) = 0;
    virtual void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) = 0;
    virtual void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance) = 0;
    virtual void Barrier(Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) = 0;
    virtual void CopyTexture(const CopyLocation& dst, uint32_t x, uint32_t y, uint32_t z, const CopyLocation& src,
                             const D3D12_BOX* box) = 0;
    virtual void CopyBuffer(Resource* dst, uint64_t dst_offset, Resource* src, uint64_t src_offset, uint64_t bytes) = 0;
    virtual void CopyResource(Resource* dst, Resource* src) = 0;
    // a multisampled colour target's samples averaged into dst
    virtual void Resolve(Resource* dst, Resource* src, DXGI_FORMAT fmt) = 0;
    virtual void Timestamp(QueryPool* q, uint32_t i) = 0;
    // n timestamps from first as 64 bit ticks into a readback buffer
    virtual void ResolveTimestamps(QueryPool* q, uint32_t first, uint32_t n, Resource* dst, uint64_t offset) = 0;
    // a texture another queue (Uploader) made and left readable: the main queue's from here
    virtual void Acquire(Resource*) {}
};

// D3D12's pipeline library, Vulkan's pipeline cache: pipelines kept by the driver across runs
class PipelineCache {
public:
    virtual ~PipelineCache() = default;
    // the pipeline from the cache (by name, made for the same description), false when it is not there
    virtual bool Load(const wchar_t* name, const PipelineDesc& d, Ref<Pipeline>& out) = 0;
    virtual bool Store(const wchar_t* name, Pipeline* p) = 0;
    virtual std::vector<uint8_t> Serialize() = 0;
};

// A queue for copies alone on a thread of its own (the remastered textures streamed in): the textures it fills (made
// in D3D12_RESOURCE_STATE_COMMON, copied into without barriers) are left readable in that state, and taken by the main
// queue (List::Acquire) once Completed is past their fence
class Uploader {
public:
    virtual ~Uploader() = default;
    virtual List* Begin(uint32_t slot) = 0;  // the list, on allocator slot (0 or 1), which the GPU is done with
    virtual uint64_t Submit() = 0;           // its fence value
    virtual uint64_t Completed() = 0;
    virtual void Wait(uint64_t value) = 0;
};

class Device {
public:
    virtual ~Device() = default;
    virtual const char* Name() const = 0;  // "D3D12", "Vulkan"
    virtual bool Vulkan() const { return false; }

    // the window's swap chain of n buffers (BGRA), presenting with tearing when the display allows it (Tearing)
    virtual bool CreateSwapChain(HWND hwnd, uint32_t w, uint32_t h, uint32_t n) = 0;
    virtual bool Tearing() const = 0;
    virtual Resource* BackBuffer(uint32_t i) = 0;
    // the buffer the frame is drawn into (Vulkan: acquired)
    virtual uint32_t CurrentBackBuffer() = 0;
    // after the GPU is idle; the back buffers' views are made again after
    virtual bool ResizeSwapChain(uint32_t w, uint32_t h) = 0;
    virtual void Present(bool vsync) = 0;

    virtual void CreateHeaps(const HeapSizes& s) = 0;
    virtual bool CreateResource(HeapType heap, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES initial,
                                const D3D12_CLEAR_VALUE* clear, Ref<Resource>& out) = 0;
    virtual void CreateRtv(Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, View at) = 0;
    virtual void CreateDsv(Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, View at) = 0;
    // a null resource: a view reading zeros
    virtual void CreateSrv(Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, View at) = 0;
    virtual void CreateSampler(const D3D12_SAMPLER_DESC& d, View at) = 0;
    // D3D12's GetCopyableFootprints: rows 256 bytes apart, subresources 512
    virtual void Footprints(const D3D12_RESOURCE_DESC& d, uint32_t first, uint32_t n, uint64_t base,
                            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT* rows, UINT64* row_bytes, UINT64* total) = 0;
    virtual bool SupportsSamples(DXGI_FORMAT f, uint32_t n) = 0;

    // any thread. profile as D3D's ("vs_5_0", "ps_5_0", "gs_5_0", "hs_5_0", "ds_5_0")
    virtual Ref<Blob> Compile(const std::string& src, const char* entry, const char* profile, std::string& error) = 0;
    virtual Ref<Pipeline> CreatePipeline(const PipelineDesc& d) = 0;  // any thread
    // the driver's cache from its saved bytes (empty: a new one); null when the device has none
    virtual std::unique_ptr<PipelineCache> OpenPipelineCache(const std::vector<uint8_t>& data) = 0;
    // what the shader cache's files are named by (shadercache/<CacheTag()>), "" for the D3D12 files
    virtual const char* CacheTag() const = 0;

    virtual Ref<QueryPool> CreateTimestamps(uint32_t n) = 0;
    virtual uint64_t TimestampFrequency() = 0;  // ticks a second

    // the main queue's one list: Submit closes it, runs it and signals the fence with value; Reset opens it again on
    // the allocator of frame slot (the GPU done with its last use)
    virtual List* MainList() = 0;
    virtual void Submit(uint64_t value) = 0;
    virtual void Reset(uint32_t slot) = 0;
    virtual uint64_t Completed() = 0;
    virtual void Wait(uint64_t value) = 0;
    // the next Submit waits for the uploader's fence value (its textures taken)
    virtual void WaitUploader(Uploader* u, uint64_t value) = 0;
    virtual std::unique_ptr<Uploader> CreateUploader() = 0;
};

std::unique_ptr<Device> CreateD3D12(bool debug);
std::unique_ptr<Device> CreateVulkan(bool debug);

// the renderer's choice (app settings: renderer = "d3d12" or "vulkan")
bool UseVulkan();

}  // namespace gpu::rhi
