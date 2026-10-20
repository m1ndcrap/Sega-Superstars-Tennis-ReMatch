// D3D12 backend of gfx

#include "gpu/native/gfx/d3d12/d3d12_gfx.h"

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

namespace sst::gfx::d3d12
{
  namespace
  {
    using Microsoft::WRL::ComPtr;

    std::wstring Wide(std::string_view s)
    {
      return std::wstring(s.begin(), s.end());
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = r;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      return b;
    }

    D3D12_RESOURCE_DESC BufferDesc(uint64_t size)
    {
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = size;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      return bd;
    }

    class D3D12Texture final : public Texture
    {
    public:
      void SetName(std::string_view name) override { resource->SetName(Wide(name).c_str()); }
      ComPtr<ID3D12Resource> resource;
    };

    class D3D12Buffer final : public Buffer
    {
    public:
      void SetName(std::string_view name) override { resource->SetName(Wide(name).c_str()); }

      const uint8_t *Map(size_t begin, size_t end) override
      {
        const D3D12_RANGE range = {begin, end};
        void *p = nullptr;

        if (FAILED(resource->Map(0, &range, &p)))
          return nullptr;

        return static_cast<const uint8_t *>(p);
      }

      void Unmap() override
      {
        const D3D12_RANGE none = {0, 0};
        resource->Unmap(0, &none);
      }

      ComPtr<ID3D12Resource> resource;
    };

    class D3D12Pipeline final : public Pipeline
    {
    public:
      ComPtr<ID3D12PipelineState> pso;
    };

    class D3D12TimestampPool final : public TimestampPool
    {
    public:
      ComPtr<ID3D12QueryHeap> heap;
    };

    class D3D12Device;

    class D3D12CommandList final : public CommandList
    {
    public:
      D3D12CommandList(D3D12Device &device) : device_(device) {}
      bool Create(uint32_t slots);

      void Begin(uint32_t slot, std::string_view name) override;
      void End() override { list_->Close(); }

      void Barrier(Texture &texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override
      {
        auto b = Transition(static_cast<D3D12Texture &>(texture).resource.Get(), before, after);
        list_->ResourceBarrier(1, &b);
      }

      void Barrier(Buffer &buffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override
      {
        auto b = Transition(static_cast<D3D12Buffer &>(buffer).resource.Get(), before, after);
        list_->ResourceBarrier(1, &b);
      }

      void ClearRtv(uint32_t rtv, const float color[4], const D3D12_RECT *rects, uint32_t rect_count) override;
      void ClearDsv(uint32_t dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil, const D3D12_RECT *rects, uint32_t rect_count) override;
      void SetTargets(uint32_t rtv, uint32_t dsv) override;

      void SetViewport(const D3D12_VIEWPORT &viewport) override { list_->RSSetViewports(1, &viewport); }
      void SetScissor(const D3D12_RECT &rect) override { list_->RSSetScissorRects(1, &rect); }
      void SetBlendFactor(const float factor[4]) override { list_->OMSetBlendFactor(factor); }

      void SetLayout(Layout layout) override;
      void SetPipeline(Pipeline &pipeline) override { list_->SetPipelineState(static_cast<D3D12Pipeline &>(pipeline).pso.Get()); }
      void SetDrawConstants(GpuAddress vs, GpuAddress ps, GpuAddress shared) override;
      void SetBlitConstants(const int32_t *values, uint32_t count) override { list_->SetGraphicsRoot32BitConstants(0, count, values, 0); }
      void SetBlitSource(uint32_t srv) override;

      void SetVertexBuffer(uint32_t slot, GpuAddress address, uint32_t size, uint32_t stride) override
      {
        const D3D12_VERTEX_BUFFER_VIEW vbv = {address, size, stride};
        list_->IASetVertexBuffers(slot, 1, &vbv);
      }

      void SetIndexBuffer(GpuAddress address, uint32_t size, DXGI_FORMAT format) override
      {
        const D3D12_INDEX_BUFFER_VIEW ibv = {address, size, format};
        list_->IASetIndexBuffer(&ibv);
      }

      void SetTopology(D3D_PRIMITIVE_TOPOLOGY topology) override { list_->IASetPrimitiveTopology(topology); }
      void Draw(uint32_t vertices, uint32_t first_vertex) override { list_->DrawInstanced(vertices, 1, first_vertex, 0); }
      void DrawIndexed(uint32_t indices, uint32_t first_index, int32_t base_vertex) override { list_->DrawIndexedInstanced(indices, 1, first_index, base_vertex, 0); }

      void CopyBufferToTexture(Texture &dst, uint32_t subresource, Buffer &src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) override
      {
        D3D12_TEXTURE_COPY_LOCATION d = {};
        d.pResource = static_cast<D3D12Texture &>(dst).resource.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION s = {};
        s.pResource = static_cast<D3D12Buffer &>(src).resource.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = footprint;
        list_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
      }

      void CopyBuffer(Buffer &dst, uint64_t dst_offset, Buffer &src, uint64_t src_offset, uint64_t size) override
      {
        list_->CopyBufferRegion(static_cast<D3D12Buffer &>(dst).resource.Get(), dst_offset, static_cast<D3D12Buffer &>(src).resource.Get(), src_offset, size);
      }

      void CopyBufferAll(Buffer &dst, Buffer &src) override
      {
        list_->CopyResource(static_cast<D3D12Buffer &>(dst).resource.Get(), static_cast<D3D12Buffer &>(src).resource.Get());
      }

      void WriteTimestamp(TimestampPool &pool, uint32_t index) override
      {
        list_->EndQuery(static_cast<D3D12TimestampPool &>(pool).heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
      }

      void ResolveTimestamps(TimestampPool &pool, uint32_t first, uint32_t count, Buffer &dst, uint64_t dst_offset) override
      {
        list_->ResolveQueryData(static_cast<D3D12TimestampPool &>(pool).heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, static_cast<D3D12Buffer &>(dst).resource.Get(), dst_offset);
      }

      void Marker(std::string_view text) override
      {
        const std::wstring wide = Wide(text);
        list_->SetMarker(0, wide.c_str(), UINT((wide.size() + 1) * sizeof(wchar_t)));
      }

      ID3D12GraphicsCommandList *list() const { return list_.Get(); }

    private:
      D3D12Device &device_;
      std::vector<ComPtr<ID3D12CommandAllocator>> allocators_;
      ComPtr<ID3D12GraphicsCommandList> list_;
    };

    class D3D12PipelineCache;

    class D3D12Device final : public Device
    {
    public:
      explicit D3D12Device(const rex::ui::d3d12::D3D12Provider &provider) : provider_(provider), device_(provider.GetDevice()), queue_(provider.GetDirectQueue()) {}

      ~D3D12Device() override
      {
        if (fence_)
          WaitFence(fence_value_);

        if (event_)
          CloseHandle(event_);
      }

      bool Initialize()
      {
        if (!device_ || !queue_ || FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
          return false;

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event_ != nullptr;
      }

      Backend backend() const override { return Backend::kD3D12; }
      ShaderFormat shader_format() const override { return ShaderFormat::kDxil; }

      TextureRef CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE *clear) override
      {
        auto t = std::make_shared<D3D12Texture>();

        if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, initial, clear, IID_PPV_ARGS(&t->resource))))
          return nullptr;

        t->desc = desc;
        return t;
      }

      BufferRef CreateBuffer(uint64_t size, HeapType heap, D3D12_RESOURCE_STATES initial) override
      {
        D3D12_HEAP_PROPERTIES props = {heap == HeapType::kUpload ? D3D12_HEAP_TYPE_UPLOAD : heap == HeapType::kReadback ? D3D12_HEAP_TYPE_READBACK
                                                                                                                        : D3D12_HEAP_TYPE_DEFAULT};

        const D3D12_RESOURCE_DESC bd = BufferDesc(size);
        auto b = std::make_shared<D3D12Buffer>();

        if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &bd, initial, nullptr, IID_PPV_ARGS(&b->resource))))
          return nullptr;

        b->size = size;

        if (heap != HeapType::kReadback)
          b->gpu = b->resource->GetGPUVirtualAddress();

        if (heap == HeapType::kUpload && FAILED(b->resource->Map(0, nullptr, reinterpret_cast<void **>(&b->cpu))))
          return nullptr;

        return b;
      }

      uint64_t TextureBytes(const D3D12_RESOURCE_DESC &desc) override { return device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes; }

      void Footprints(const D3D12_RESOURCE_DESC &desc, uint32_t first, uint32_t count, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints, uint32_t *rows, uint64_t *row_bytes, uint64_t *total) override
      {
        device_->GetCopyableFootprints(&desc, first, count, 0, footprints, rows, row_bytes, total);
      }

      bool SupportsSamples(DXGI_FORMAT format, uint32_t samples) override
      {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {};
        q.Format = format;
        q.SampleCount = samples;
        return SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels > 0;
      }

      bool CreateTables(const TableSizes &sizes) override
      {
        auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool visible, ComPtr<ID3D12DescriptorHeap> &out, uint32_t &increment)
        {
          D3D12_DESCRIPTOR_HEAP_DESC hd = {};
          hd.Type = type;
          hd.NumDescriptors = std::max(count, 1u);
          hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
          increment = device_->GetDescriptorHandleIncrementSize(type);
          return SUCCEEDED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out)));
        };

        return heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, sizes.srv, true, srv_heap_, srv_size_) && heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sizes.sampler, true, sampler_heap_, sampler_size_) && heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, sizes.rtv, false, rtv_heap_, rtv_size_) && heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, sizes.dsv, false, dsv_heap_, dsv_size_);
      }

      void WriteSrv(uint32_t slot, Texture &texture, const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) override
      {
        device_->CreateShaderResourceView(static_cast<D3D12Texture &>(texture).resource.Get(), &desc, Cpu(srv_heap_.Get(), srv_size_, slot));
      }

      void WriteSampler(uint32_t slot, const D3D12_SAMPLER_DESC &desc) override
      {
        device_->CreateSampler(&desc, Cpu(sampler_heap_.Get(), sampler_size_, slot));
      }

      void WriteRtv(uint32_t slot, Texture &texture, const D3D12_RENDER_TARGET_VIEW_DESC *desc) override
      {
        device_->CreateRenderTargetView(static_cast<D3D12Texture &>(texture).resource.Get(), desc, Rtv(slot));
      }

      void WriteDsv(uint32_t slot, Texture &texture, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc) override
      {
        device_->CreateDepthStencilView(static_cast<D3D12Texture &>(texture).resource.Get(), desc, Dsv(slot));
      }

      bool CreateLayouts() override { return CreateDrawRoot() && CreateBlitRoot(); }

      PipelineRef CreatePipeline(const PipelineDesc &desc, int32_t *error) override
      {
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = Translate(desc, layout);
        auto p = std::make_shared<D3D12Pipeline>();
        const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p->pso));

        if (error)
          *error = int32_t(hr);

        return SUCCEEDED(hr) ? p : nullptr;
      }

      std::unique_ptr<PipelineCache> OpenPipelineCache(const std::filesystem::path &path) override;

      TimestampPoolRef CreateTimestampPool(uint32_t count) override
      {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = count;
        auto p = std::make_shared<D3D12TimestampPool>();

        if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&p->heap))))
          return nullptr;

        return p;
      }

      uint64_t TimestampFrequency() override
      {
        uint64_t f = 0;
        return SUCCEEDED(queue_->GetTimestampFrequency(&f)) ? f : 0;
      }

      std::unique_ptr<CommandList> CreateCommandList(uint32_t slots) override
      {
        auto l = std::make_unique<D3D12CommandList>(*this);
        return l->Create(slots) ? std::move(l) : nullptr;
      }

      uint64_t Submit(CommandList &list) override
      {
        ID3D12CommandList *lists[] = {static_cast<D3D12CommandList &>(list).list()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fence_value_);
        return fence_value_;
      }

      uint64_t CompletedFence() override { return fence_->GetCompletedValue(); }

      void WaitFence(uint64_t value) override
      {
        if (!value || fence_->GetCompletedValue() >= value)
          return;

        if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
          WaitForSingleObject(event_, INFINITE);
      }

      uint64_t LastSubmitted() const override { return fence_value_; }

      // For the command list
      D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t slot) const { return Cpu(rtv_heap_.Get(), rtv_size_, slot); }
      D3D12_CPU_DESCRIPTOR_HANDLE Dsv(uint32_t slot) const { return Cpu(dsv_heap_.Get(), dsv_size_, slot); }

      D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(uint32_t slot) const
      {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += size_t(slot) * srv_size_;
        return h;
      }

      ID3D12DescriptorHeap *srv_heap() const { return srv_heap_.Get(); }
      ID3D12DescriptorHeap *sampler_heap() const { return sampler_heap_.Get(); }
      ID3D12RootSignature *root(Layout layout) const { return layout == Layout::kDraw ? draw_root_.Get() : blit_root_.Get(); }
      ID3D12Device *device() const { return device_; }
      ID3D12CommandQueue *queue() const { return queue_; }

      D3D12_GRAPHICS_PIPELINE_STATE_DESC Translate(const PipelineDesc &desc, std::vector<D3D12_INPUT_ELEMENT_DESC> &layout) const
      {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = root(desc.layout);
        d.VS = {desc.vs.data, desc.vs.size};

        if (desc.ps)
          d.PS = {desc.ps.data, desc.ps.size};

        d.BlendState = desc.blend;
        d.SampleMask = UINT_MAX;
        d.RasterizerState = desc.raster;
        d.DepthStencilState = desc.depth;
        layout.clear();

        for (const InputElement &e : desc.inputs)
          layout.push_back(e.d3d);

        d.InputLayout = {layout.data(), UINT(layout.size())};
        d.IBStripCutValue = desc.strip_cut;
        d.PrimitiveTopologyType = desc.topology;

        if (desc.rtv != DXGI_FORMAT_UNKNOWN)
        {
          d.NumRenderTargets = 1;
          d.RTVFormats[0] = desc.rtv;
        }

        d.DSVFormat = desc.dsv;
        d.SampleDesc.Count = desc.samples;
        return d;
      }

    private:
      static D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap *heap, uint32_t increment, uint32_t slot)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(slot) * increment;
        return h;
      }

      bool Serialize(const D3D12_ROOT_SIGNATURE_DESC &rs, ComPtr<ID3D12RootSignature> &out)
      {
        ComPtr<ID3DBlob> blob, error;

        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        {
          if (error)
            REXLOG_ERROR("[sst-native] root signature: {}", std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize()));

          return false;
        }

        return SUCCEEDED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)));
      }

      // XenosRecomp's D3D12 layout: root CBVs b0-b2 space4 (VS / PS / shared constants), b3-b4 space4 (the VS / PS constants again, read as an array for relative addressing), unbounded SRV tables t0 spaces 0-2 (2D / 3D / cube) and samplers s0 space3
      bool CreateDrawRoot()
      {
        D3D12_ROOT_PARAMETER params[9] = {};

        for (UINT i = 0; i < 5; ++i)
        {
          params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
          params[i].Descriptor.ShaderRegister = i;
          params[i].Descriptor.RegisterSpace = 4;
          params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_DESCRIPTOR_RANGE ranges[4] = {};

        for (UINT i = 0; i < 4; ++i)
        {
          ranges[i].RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
          ranges[i].NumDescriptors = UINT_MAX; // unbounded (resource binding tier 2+)
          ranges[i].BaseShaderRegister = 0;
          ranges[i].RegisterSpace = i;
          ranges[i].OffsetInDescriptorsFromTableStart = 0;
          D3D12_ROOT_PARAMETER &p = params[5 + i];
          p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
          p.DescriptorTable.NumDescriptorRanges = 1;
          p.DescriptorTable.pDescriptorRanges = &ranges[i];
          p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 9;
        rs.pParameters = params;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; // F9 capture
        return Serialize(rs, draw_root_);
      }

      // b0 = 8 root constants (source offset, flags, gamma; source rectangle for FXAA), t0 = source, s0 = linear clamp
      bool CreateBlitRoot()
      {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 8};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC linear = {};
        linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linear.MaxLOD = D3D12_FLOAT32_MAX;
        linear.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &linear;
        return Serialize(rs, blit_root_);
      }

      const rex::ui::d3d12::D3D12Provider &provider_;
      ID3D12Device *device_;
      ID3D12CommandQueue *queue_;
      ComPtr<ID3D12Fence> fence_;
      HANDLE event_ = nullptr;
      uint64_t fence_value_ = 0;
      ComPtr<ID3D12DescriptorHeap> srv_heap_, sampler_heap_, rtv_heap_, dsv_heap_;
      uint32_t srv_size_ = 0, sampler_size_ = 0, rtv_size_ = 0, dsv_size_ = 0;
      ComPtr<ID3D12RootSignature> draw_root_, blit_root_;
    };

    bool D3D12CommandList::Create(uint32_t slots)
    {
      allocators_.resize(slots);

      for (auto &a : allocators_)
      {
        if (FAILED(device_.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
          return false;
      }

      if (FAILED(device_.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_))))
        return false;

      list_->Close();
      return true;
    }

    void D3D12CommandList::Begin(uint32_t slot, std::string_view name)
    {
      ID3D12CommandAllocator *a = allocators_[slot % allocators_.size()].Get();
      a->Reset();
      list_->Reset(a, nullptr);

      if (!name.empty())
        list_->SetName(Wide(name).c_str());

      if (device_.srv_heap())
      {
        ID3D12DescriptorHeap *heaps[] = {device_.srv_heap(), device_.sampler_heap()};
        list_->SetDescriptorHeaps(2, heaps);
      }
    }

    void D3D12CommandList::ClearRtv(uint32_t rtv, const float color[4], const D3D12_RECT *rects, uint32_t rect_count)
    {
      list_->ClearRenderTargetView(device_.Rtv(rtv), color, rect_count, rect_count ? rects : nullptr);
    }

    void D3D12CommandList::ClearDsv(uint32_t dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil, const D3D12_RECT *rects, uint32_t rect_count)
    {
      list_->ClearDepthStencilView(device_.Dsv(dsv), flags, depth, stencil, rect_count, rect_count ? rects : nullptr);
    }

    void D3D12CommandList::SetTargets(uint32_t rtv, uint32_t dsv)
    {
      const D3D12_CPU_DESCRIPTOR_HANDLE r = rtv != kNoView ? device_.Rtv(rtv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
      const D3D12_CPU_DESCRIPTOR_HANDLE d = dsv != kNoView ? device_.Dsv(dsv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
      list_->OMSetRenderTargets(rtv != kNoView ? 1 : 0, rtv != kNoView ? &r : nullptr, FALSE, dsv != kNoView ? &d : nullptr);
    }

    void D3D12CommandList::SetLayout(Layout layout)
    {
      list_->SetGraphicsRootSignature(device_.root(layout));

      if (layout == Layout::kDraw)
      {
        // (every table starts at the heap's start: the shaders index the whole heap)
        for (UINT i = 0; i < 3; ++i)
          list_->SetGraphicsRootDescriptorTable(5 + i, device_.srv_heap()->GetGPUDescriptorHandleForHeapStart());

        list_->SetGraphicsRootDescriptorTable(8, device_.sampler_heap()->GetGPUDescriptorHandleForHeapStart());
      }
    }

    void D3D12CommandList::SetDrawConstants(GpuAddress vs, GpuAddress ps, GpuAddress shared)
    {
      // Root parameters: 0 VS constants, 1 PS constants, 2 shared, 3 / 4 the VS / PS constants as raw arrays
      list_->SetGraphicsRootConstantBufferView(0, vs);
      list_->SetGraphicsRootConstantBufferView(3, vs);
      list_->SetGraphicsRootConstantBufferView(1, ps);
      list_->SetGraphicsRootConstantBufferView(4, ps);
      list_->SetGraphicsRootConstantBufferView(2, shared);
    }

    void D3D12CommandList::SetBlitSource(uint32_t srv)
    {
      list_->SetGraphicsRootDescriptorTable(1, device_.SrvGpu(srv));
    }

    // ID3D12PipelineLibrary in a file (cache/pipelines*.bin). Rejected files (another driver) are replaced
    class D3D12PipelineCache final : public PipelineCache
    {
    public:
      D3D12PipelineCache(D3D12Device &device, std::filesystem::path path) : device_(device), path_(std::move(path)) {}

      bool Open()
      {
        ComPtr<ID3D12Device1> device1;

        if (FAILED(device_.device()->QueryInterface(IID_PPV_ARGS(&device1))))
          return false;

        {
          std::ifstream in(path_, std::ios::binary);

          if (in)
            blob_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        HRESULT hr = E_FAIL;

        if (!blob_.empty())
        {
          hr = device1->CreatePipelineLibrary(blob_.data(), blob_.size(), IID_PPV_ARGS(&library_));

          if (FAILED(hr))
            REXLOG_INFO("[sst-native] pipeline cache {} not usable ({:08X}: new driver?); starting a new one", path_.string(), uint32_t(hr));
        }

        if (!library_)
        {
          blob_.clear();
          hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library_));

          if (FAILED(hr))
          {
            library_.Reset();
            REXLOG_WARN("[sst-native] pipeline cache unavailable ({:08X})", uint32_t(hr));
            return false;
          }
        }

        file_bytes_ = blob_.size();
        REXLOG_INFO("[sst-native] pipeline cache: {} ({} KB)", path_.string(), blob_.size() >> 10);
        return true;
      }

      PipelineRef Load(const std::wstring &name, const PipelineDesc &desc) override
      {
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = device_.Translate(desc, layout);
        auto p = std::make_shared<D3D12Pipeline>();
        std::lock_guard lock(mutex_);

        if (FAILED(library_->LoadGraphicsPipeline(name.c_str(), &d, IID_PPV_ARGS(&p->pso))))
          return nullptr;

        return p;
      }

      PipelineRef Create(const std::wstring &name, const PipelineDesc &desc, int32_t *error) override
      {
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = device_.Translate(desc, layout);
        auto p = std::make_shared<D3D12Pipeline>();
        const HRESULT hr = device_.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p->pso));

        if (error)
          *error = int32_t(hr);

        if (FAILED(hr))
          return nullptr;

        std::lock_guard lock(mutex_);

        if (SUCCEEDED(library_->StorePipeline(name.c_str(), p->pso.Get())))
          dirty_ = true;

        return p;
      }

      size_t Save() override
      {
        std::vector<uint8_t> data;

        {
          std::lock_guard lock(mutex_);

          if (!dirty_)
            return 0;

          dirty_ = false;
          data.resize(library_->GetSerializedSize());

          if (data.empty() || FAILED(library_->Serialize(data.data(), data.size())))
            return 0;
        }

        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        const auto tmp = std::filesystem::path(path_).concat(".tmp");

        {
          std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

          if (!out)
            return 0;

          out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));

          if (!out)
            return 0;
        }

        std::filesystem::rename(tmp, path_, ec);
        return ec ? 0 : data.size();
      }

      bool Dirty() const override
      {
        std::lock_guard lock(mutex_);
        return dirty_;
      }

      size_t FileBytes() const override { return file_bytes_; }

    private:
      D3D12Device &device_;
      std::filesystem::path path_;
      ComPtr<ID3D12PipelineLibrary> library_;
      std::vector<uint8_t> blob_; // must outlive the library
      mutable std::mutex mutex_;
      bool dirty_ = false;
      size_t file_bytes_ = 0;
    };

    std::unique_ptr<PipelineCache> D3D12Device::OpenPipelineCache(const std::filesystem::path &path)
    {
      auto cache = std::make_unique<D3D12PipelineCache>(*this, path);
      return cache->Open() ? std::move(cache) : nullptr;
    }
  }

  std::unique_ptr<Device> CreateDevice(const rex::ui::d3d12::D3D12Provider &provider)
  {
    auto device = std::make_unique<D3D12Device>(provider);
    return device->Initialize() ? std::move(device) : nullptr;
  }

  ID3D12Device *NativeDevice(Device &device)
  {
    auto *d = dynamic_cast<D3D12Device *>(&device);
    return d ? d->device() : nullptr;
  }

  ID3D12CommandQueue *NativeQueue(Device &device)
  {
    auto *d = dynamic_cast<D3D12Device *>(&device);
    return d ? d->queue() : nullptr;
  }

  ID3D12GraphicsCommandList *NativeList(CommandList &list)
  {
    auto *l = dynamic_cast<D3D12CommandList *>(&list);
    return l ? l->list() : nullptr;
  }

  ID3D12Resource *NativeResource(Texture &texture)
  {
    auto *t = dynamic_cast<D3D12Texture *>(&texture);
    return t ? t->resource.Get() : nullptr;
  }

  ID3D12Resource *NativeResource(Buffer &buffer)
  {
    auto *b = dynamic_cast<D3D12Buffer *>(&buffer);
    return b ? b->resource.Get() : nullptr;
  }

  ID3D12RootSignature *NativeRootSignature(Device &device, Layout layout)
  {
    auto *d = dynamic_cast<D3D12Device *>(&device);
    return d ? d->root(layout) : nullptr;
  }
}