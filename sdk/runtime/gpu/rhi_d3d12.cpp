// The renderer's graphics API (gpu/rhi.h) done by Direct3D 12: each call the one it stands for
#include "gpu/rhi.h"

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

#include "core/log.h"

namespace gpu::rhi {
namespace {

using Microsoft::WRL::ComPtr;

class DxResource final : public Resource {
public:
    DxResource(ComPtr<ID3D12Resource> r, HeapType heap) : r_(std::move(r)) {
        heap_ = heap;
        desc_ = r_->GetDesc();
        if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) va_ = r_->GetGPUVirtualAddress();
        if (heap != HeapType::Default) r_->Map(0, nullptr, reinterpret_cast<void**>(&map_));
    }
    GpuVa Va() const override { return va_; }
    uint8_t* Map() override { return map_; }
    void SetName(const wchar_t* n) override { r_->SetName(n); }
    ID3D12Resource* Get() const { return r_.Get(); }

private:
    ComPtr<ID3D12Resource> r_;
    GpuVa va_ = 0;
    uint8_t* map_ = nullptr;
};

ID3D12Resource* Dx(Resource* r) { return r ? static_cast<DxResource*>(r)->Get() : nullptr; }

class DxPipeline final : public Pipeline {
public:
    explicit DxPipeline(ComPtr<ID3D12PipelineState> p) : p(std::move(p)) {}
    ComPtr<ID3D12PipelineState> p;
};

class DxQueries final : public QueryPool {
public:
    ComPtr<ID3D12QueryHeap> heap;
};

D3D12_SHADER_BYTECODE Code(const Blob* b) { return b ? D3D12_SHADER_BYTECODE{b->GetBufferPointer(), b->GetBufferSize()} : D3D12_SHADER_BYTECODE{}; }

// the views' tables
struct DxHeap {
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t inc = 0;
};

class DxDevice;

class DxList final : public List {
public:
    DxDevice& d;
    ComPtr<ID3D12GraphicsCommandList> cl;
    explicit DxList(DxDevice& dev) : d(dev) {}
    void Open();  // its heaps and the main layout, after a reset
    void SetLayout(Layout l) override;
    void SetPipeline(Pipeline* p) override { cl->SetPipelineState(static_cast<DxPipeline*>(p)->p.Get()); }
    void SetCbv(uint32_t param, GpuVa va) override { cl->SetGraphicsRootConstantBufferView(param, va); }
    void SetTable(uint32_t param, Table t) override;
    void SetViewport(const D3D12_VIEWPORT& v) override { cl->RSSetViewports(1, &v); }
    void SetScissor(const D3D12_RECT& r) override { cl->RSSetScissorRects(1, &r); }
    void SetBlendFactor(const float f[4]) override { cl->OMSetBlendFactor(f); }
    void SetStencilRef(uint32_t r) override { cl->OMSetStencilRef(r); }
    void SetTopology(D3D_PRIMITIVE_TOPOLOGY t) override { cl->IASetPrimitiveTopology(t); }
    void SetVertexBuffers(uint32_t start, uint32_t n, const D3D12_VERTEX_BUFFER_VIEW* v) override { cl->IASetVertexBuffers(start, n, v); }
    void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) override { cl->IASetIndexBuffer(v); }
    void SetTargets(uint32_t n, const View* rtv, const View* dsv) override;
    void ClearRtv(View v, const float color[4], uint32_t n, const D3D12_RECT* rects) override;
    void ClearDsv(View v, D3D12_CLEAR_FLAGS flags, float z, uint8_t stencil, uint32_t n, const D3D12_RECT* rects) override;
    void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) override {
        cl->DrawInstanced(count, instances, first, first_instance);
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance) override {
        cl->DrawIndexedInstanced(count, instances, first, base, first_instance);
    }
    void Barrier(Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = Dx(r);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cl->ResourceBarrier(1, &b);
    }
    void CopyTexture(const CopyLocation& dst, uint32_t x, uint32_t y, uint32_t z, const CopyLocation& src,
                     const D3D12_BOX* box) override {
        auto loc = [](const CopyLocation& c) {
            D3D12_TEXTURE_COPY_LOCATION l{Dx(c.pResource), c.Type};
            if (c.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) l.PlacedFootprint = c.PlacedFootprint;
            else l.SubresourceIndex = c.SubresourceIndex;
            return l;
        };
        D3D12_TEXTURE_COPY_LOCATION d = loc(dst), s = loc(src);
        cl->CopyTextureRegion(&d, x, y, z, &s, box);
    }
    void CopyBuffer(Resource* dst, uint64_t dst_offset, Resource* src, uint64_t src_offset, uint64_t bytes) override {
        cl->CopyBufferRegion(Dx(dst), dst_offset, Dx(src), src_offset, bytes);
    }
    void CopyResource(Resource* dst, Resource* src) override { cl->CopyResource(Dx(dst), Dx(src)); }
    void Resolve(Resource* dst, Resource* src, DXGI_FORMAT fmt) override { cl->ResolveSubresource(Dx(dst), 0, Dx(src), 0, fmt); }
    void Timestamp(QueryPool* q, uint32_t i) override {
        cl->EndQuery(static_cast<DxQueries*>(q)->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
    }
    void ResolveTimestamps(QueryPool* q, uint32_t first, uint32_t n, Resource* dst, uint64_t offset) override {
        cl->ResolveQueryData(static_cast<DxQueries*>(q)->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, n, Dx(dst), offset);
    }
};

// D3D12's pipeline library: pipelines by name, made from the same description
class DxPipelineCache final : public PipelineCache {
public:
    ComPtr<ID3D12PipelineLibrary> lib;
    std::vector<uint8_t> data;  // kept alive: the library reads from it
    DxDevice& d;
    explicit DxPipelineCache(DxDevice& dev) : d(dev) {}
    bool Load(const wchar_t* name, const PipelineDesc& pd, Ref<Pipeline>& out) override;
    bool Store(const wchar_t* name, Pipeline* p) override {
        return SUCCEEDED(lib->StorePipeline(name, static_cast<DxPipeline*>(p)->p.Get()));
    }
    std::vector<uint8_t> Serialize() override {
        std::vector<uint8_t> out(lib->GetSerializedSize());
        if (FAILED(lib->Serialize(out.data(), out.size()))) out.clear();
        return out;
    }
};

class DxUploader final : public Uploader {
public:
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[2];
    DxList list;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE event = nullptr;
    explicit DxUploader(DxDevice& d) : list(d) {}
    ~DxUploader() override {
        if (event) CloseHandle(event);
    }
    List* Begin(uint32_t slot) override {
        alloc[slot]->Reset();
        list.cl->Reset(alloc[slot].Get(), nullptr);
        return &list;
    }
    uint64_t Submit() override {
        list.cl->Close();
        ID3D12CommandList* lists[] = {list.cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        return value;
    }
    uint64_t Completed() override { return fence->GetCompletedValue(); }
    void Wait(uint64_t v) override {
        if (fence->GetCompletedValue() >= v) return;
        fence->SetEventOnCompletion(v, event);
        WaitForSingleObject(event, INFINITE);
    }
};

class DxDevice final : public Device {
public:
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    std::vector<Ref<Resource>> bb;
    bool tearing = false;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    ComPtr<ID3D12CommandAllocator> alloc[4];
    DxList list{*this};
    DxHeap heaps[4];  // by Heap
    ComPtr<ID3D12RootSignature> root[2];  // by Layout

    ~DxDevice() override {
        if (event) CloseHandle(event);
    }
    const char* Name() const override { return "D3D12"; }

    bool Init(bool debug) {
        if (debug) {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        }
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        // dedicated GPU first on hybrid laptops
        ComPtr<IDXGIFactory6> f6;
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory.As(&f6))) f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
            FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            LOGE("d3d12: no device");
            return false;
        }
        DXGI_ADAPTER_DESC1 ad{};
        if (adapter) adapter->GetDesc1(&ad);
        LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        ComPtr<IDXGIFactory5> f5;
        BOOL tear = FALSE;
        if (SUCCEEDED(factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
        tearing = tear;
        for (auto& a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list.cl));
        return CreateRoots();
    }

    bool CreateRoots() {
        // Main
        {
            D3D12_DESCRIPTOR_RANGE ranges[2] = {};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = 7;  // the stages, then a remastered material's normal, material and detail maps
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            ranges[1].NumDescriptors = 4;
            D3D12_ROOT_PARAMETER params[6] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor.ShaderRegister = 0;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[1].Descriptor.ShaderRegister = 1;
            for (int i = 0; i < 2; i++) {
                params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
                params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
                params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }
            // t7: the sun's reach on the screen, t8: the wave field (Water Interaction), t9: the snow's trail
            D3D12_DESCRIPTOR_RANGE sun{};
            sun.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            sun.NumDescriptors = 3;
            sun.BaseShaderRegister = 7;
            params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[4].DescriptorTable.NumDescriptorRanges = 1;
            params[4].DescriptorTable.pDescriptorRanges = &sun;
            params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            // b2: the top view's constants (WaterInteraction)
            params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[5].Descriptor.ShaderRegister = 2;
            // s4: the wave field's, linear and clamped; s5: trilinear and repeating, no anisotropy (the relief's
            // height walk: many taps a pixel, anisotropy there costs much and shows nothing)
            // s6: the materials' own maps (normal, material, detail), anisotropic at 4 and repeating (the stage's
            // 16 over several maps a pixel costs much on a driver that does not trim it, and shows little)
            D3D12_STATIC_SAMPLER_DESC ss[3]{};
            ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[0].MaxLOD = D3D12_FLOAT32_MAX;
            ss[0].ShaderRegister = 4;
            ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            ss[1] = ss[0];
            ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            ss[1].ShaderRegister = 5;
            ss[2] = ss[1];
            ss[2].Filter = D3D12_FILTER_ANISOTROPIC;
            ss[2].MaxAnisotropy = 4;
            ss[2].ShaderRegister = 6;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 6;
            rsd.pParameters = params;
            rsd.NumStaticSamplers = 3;
            rsd.pStaticSamplers = ss;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            if (!MakeRoot(rsd, root[0])) return false;
        }
        // Motion
        {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER params[6] = {};
            for (int i = 0; i < 5; i++) params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, params[i].Descriptor.ShaderRegister = UINT(i);
            params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[5].DescriptorTable.NumDescriptorRanges = 1;
            params[5].DescriptorTable.pDescriptorRanges = &range;
            params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 6;
            rsd.pParameters = params;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            if (!MakeRoot(rsd, root[1])) return false;
        }
        return true;
    }
    bool MakeRoot(const D3D12_ROOT_SIGNATURE_DESC& rsd, ComPtr<ID3D12RootSignature>& out) {
        ComPtr<ID3DBlob> rs, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
        return SUCCEEDED(dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&out)));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(View v) const {
        D3D12_CPU_DESCRIPTOR_HANDLE c = heaps[int(v.heap)].cpu;
        c.ptr += size_t(v.index) * heaps[int(v.heap)].inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(Table t) const {
        D3D12_GPU_DESCRIPTOR_HANDLE g = heaps[int(t.heap)].gpu;
        g.ptr += uint64_t(t.index) * heaps[int(t.heap)].inc;
        return g;
    }

    bool CreateSwapChain(HWND hwnd, uint32_t w, uint32_t h, uint32_t n) override {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = n;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
            LOGE("d3d12: no swap chain");
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        sc1.As(&swap);
        bb.resize(n);
        GetBuffers();
        return true;
    }
    void GetBuffers() {
        for (uint32_t i = 0; i < bb.size(); i++) {
            ComPtr<ID3D12Resource> r;
            swap->GetBuffer(i, IID_PPV_ARGS(&r));
            bb[i] = Ref<Resource>::Adopt(new DxResource(r, HeapType::Default));
        }
    }
    bool Tearing() const override { return tearing; }
    Resource* BackBuffer(uint32_t i) override { return bb[i].Get(); }
    uint32_t CurrentBackBuffer() override { return swap->GetCurrentBackBufferIndex(); }
    bool ResizeSwapChain(uint32_t w, uint32_t h) override {
        for (auto& b : bb) b.Reset();
        HRESULT hr = swap->ResizeBuffers(UINT(bb.size()), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        GetBuffers();
        return SUCCEEDED(hr);
    }
    void Present(bool vsync) override { swap->Present(vsync ? 1 : 0, !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0); }

    void CreateHeaps(const HeapSizes& s) override {
        auto make = [&](Heap h, D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible) {
            DxHeap& x = heaps[int(h)];
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = t;
            hd.NumDescriptors = n;
            hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&x.heap));
            x.inc = dev->GetDescriptorHandleIncrementSize(t);
            x.cpu = x.heap->GetCPUDescriptorHandleForHeapStart();
            if (visible) x.gpu = x.heap->GetGPUDescriptorHandleForHeapStart();
        };
        make(Heap::Srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s.srv, true);
        make(Heap::Sampler, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s.sampler, true);
        make(Heap::Rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, s.rtv, false);
        make(Heap::Dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, s.dsv, false);
        list.Open();
    }

    bool CreateResource(HeapType heap, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES initial,
                        const D3D12_CLEAR_VALUE* clear, Ref<Resource>& out) override {
        D3D12_HEAP_PROPERTIES hp{heap == HeapType::Upload ? D3D12_HEAP_TYPE_UPLOAD
                                 : heap == HeapType::Readback ? D3D12_HEAP_TYPE_READBACK
                                                              : D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, clear, IID_PPV_ARGS(&r)))) {
            out.Reset();
            return false;
        }
        out = Ref<Resource>::Adopt(new DxResource(std::move(r), heap));
        return true;
    }
    void CreateRtv(Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, View at) override { dev->CreateRenderTargetView(Dx(r), d, Cpu(at)); }
    void CreateDsv(Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, View at) override { dev->CreateDepthStencilView(Dx(r), d, Cpu(at)); }
    void CreateSrv(Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, View at) override {
        dev->CreateShaderResourceView(Dx(r), d, Cpu(at));
    }
    void CreateSampler(const D3D12_SAMPLER_DESC& d, View at) override { dev->CreateSampler(&d, Cpu(at)); }
    void Footprints(const D3D12_RESOURCE_DESC& d, uint32_t first, uint32_t n, uint64_t base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,
                    UINT* rows, UINT64* row_bytes, UINT64* total) override {
        dev->GetCopyableFootprints(&d, first, n, base, fp, rows, row_bytes, total);
    }
    bool SupportsSamples(DXGI_FORMAT f, uint32_t n) override {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = f, q.SampleCount = n;
        return SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels > 0;
    }

    Ref<Blob> Compile(const std::string& src, const char* entry, const char* profile, std::string& error) override {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> err;
        // (hull shaders unoptimised: the optimiser splits the patch constants into phases its own validator rejects)
        UINT flags = std::strncmp(profile, "hs_", 3) == 0 ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1;
        HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile, flags, 0, &code, &err);
        if (FAILED(hr)) {
            error = err ? static_cast<const char*>(err->GetBufferPointer()) : "?";
            return nullptr;
        }
        return Ref<Blob>::Adopt(new Blob(code->GetBufferPointer(), code->GetBufferSize()));
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Native(const PipelineDesc& p) const {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root[int(p.layout)].Get();
        d.VS = Code(p.VS), d.PS = Code(p.PS), d.GS = Code(p.GS), d.HS = Code(p.HS), d.DS = Code(p.DS);
        d.BlendState = p.BlendState;
        d.SampleMask = p.SampleMask;
        d.RasterizerState = p.RasterizerState;
        d.DepthStencilState = p.DepthStencilState;
        d.InputLayout = p.InputLayout;
        d.PrimitiveTopologyType = p.PrimitiveTopologyType;
        d.NumRenderTargets = p.NumRenderTargets;
        std::memcpy(d.RTVFormats, p.RTVFormats, sizeof(d.RTVFormats));
        d.DSVFormat = p.DSVFormat;
        d.SampleDesc = p.SampleDesc;
        return d;
    }
    Ref<Pipeline> CreatePipeline(const PipelineDesc& p) override {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = Native(p);
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
            // with the debug layer (XBE_D3D12_DEBUG), why
            ComPtr<ID3D12InfoQueue> q;
            if (SUCCEEDED(dev.As(&q))) {
                for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; i++) {
                    SIZE_T len = 0;
                    q->GetMessage(i, nullptr, &len);
                    std::vector<uint8_t> m(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(m.data());
                    if (SUCCEEDED(q->GetMessage(i, msg, &len))) LOGE("d3d12: %s", msg->pDescription);
                }
                q->ClearStoredMessages();
            }
            return nullptr;
        }
        return Ref<Pipeline>::Adopt(new DxPipeline(std::move(pso)));
    }
    std::unique_ptr<PipelineCache> OpenPipelineCache(const std::vector<uint8_t>& data) override {
        ComPtr<ID3D12Device1> dev1;
        if (FAILED(dev.As(&dev1))) return nullptr;
        auto c = std::make_unique<DxPipelineCache>(*this);
        c->data = data;
        if (c->data.empty() || FAILED(dev1->CreatePipelineLibrary(c->data.data(), c->data.size(), IID_PPV_ARGS(&c->lib)))) {
            c->data.clear();
            if (FAILED(dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&c->lib)))) return nullptr;
        }
        return c;
    }
    const char* CacheTag() const override { return ""; }

    Ref<QueryPool> CreateTimestamps(uint32_t n) override {
        auto q = Ref<QueryPool>::Adopt(new DxQueries);
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, n, 0};
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&static_cast<DxQueries*>(q.Get())->heap)))) return nullptr;
        return q;
    }
    uint64_t TimestampFrequency() override {
        UINT64 f = 0;
        queue->GetTimestampFrequency(&f);
        return f;
    }

    List* MainList() override { return &list; }
    void Submit(uint64_t value) override {
        list.cl->Close();
        ID3D12CommandList* lists[] = {list.cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), value);
    }
    void Reset(uint32_t slot) override {
        alloc[slot]->Reset();
        list.cl->Reset(alloc[slot].Get(), nullptr);
        list.Open();
    }
    uint64_t Completed() override { return fence->GetCompletedValue(); }
    void Wait(uint64_t value) override {
        if (fence->GetCompletedValue() >= value) return;
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, INFINITE);
    }
    // (D3D12: the textures' fences were seen passed by the CPU before they are taken, and the direct queue promotes
    // them from the common state)
    void WaitUploader(Uploader*, uint64_t) override {}
    std::unique_ptr<Uploader> CreateUploader() override {
        auto u = std::make_unique<DxUploader>(*this);
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_COPY};
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&u->queue)))) return nullptr;
        for (auto& a : u->alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, u->alloc[0].Get(), nullptr, IID_PPV_ARGS(&u->list.cl));
        u->list.cl->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&u->fence));
        u->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return u;
    }
};

void DxList::Open() {
    ID3D12DescriptorHeap* h[] = {d.heaps[int(Heap::Srv)].heap.Get(), d.heaps[int(Heap::Sampler)].heap.Get()};
    if (h[0] && h[1]) cl->SetDescriptorHeaps(2, h);
    cl->SetGraphicsRootSignature(d.root[0].Get());
}
void DxList::SetLayout(Layout l) { cl->SetGraphicsRootSignature(d.root[int(l)].Get()); }
void DxList::SetTable(uint32_t param, Table t) { cl->SetGraphicsRootDescriptorTable(param, d.Gpu(t)); }
void DxList::SetTargets(uint32_t n, const View* rtv, const View* dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE rh[8];
    for (uint32_t i = 0; i < n; i++) rh[i] = d.Cpu(rtv[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE dh{};
    if (dsv) dh = d.Cpu(*dsv);
    cl->OMSetRenderTargets(n, n ? rh : nullptr, FALSE, dsv ? &dh : nullptr);
}
void DxList::ClearRtv(View v, const float color[4], uint32_t n, const D3D12_RECT* rects) {
    cl->ClearRenderTargetView(d.Cpu(v), color, n, rects);
}
void DxList::ClearDsv(View v, D3D12_CLEAR_FLAGS flags, float z, uint8_t stencil, uint32_t n, const D3D12_RECT* rects) {
    cl->ClearDepthStencilView(d.Cpu(v), flags, z, stencil, n, rects);
}

bool DxPipelineCache::Load(const wchar_t* name, const PipelineDesc& pd, Ref<Pipeline>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC n = d.Native(pd);
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(lib->LoadGraphicsPipeline(name, &n, IID_PPV_ARGS(&pso)))) return false;
    out = Ref<Pipeline>::Adopt(new DxPipeline(std::move(pso)));
    return true;
}

}  // namespace

std::unique_ptr<Device> CreateD3D12(bool debug) {
    auto d = std::make_unique<DxDevice>();
    if (!d->Init(debug)) return nullptr;
    return d;
}

}  // namespace gpu::rhi
