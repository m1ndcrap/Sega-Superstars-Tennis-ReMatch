// The renderer describes resources, views and state with D3D12 / DXGI plain-data types and records its work through gfx::Device and gfx::CommandList, which a backend implements (D3D12 or Vulkan)

#pragma once

#include "gpu/native/gfx/d3d_vocab.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sst::gfx
{
  using GpuAddress = uint64_t;

  enum class Backend : uint8_t
  {
    kD3D12,
    kVulkan
  };

  enum class ShaderFormat : uint8_t
  {
    kDxil,
    kSpirv
  };

  enum class HeapType : uint8_t
  {
    kDefault,
    kUpload,   // CPU-written, persistently mapped (Buffer::cpu)
    kReadback, // GPU-written, read with Buffer::Map
  };

  class Resource
  {
  public:
    virtual ~Resource() = default;
    virtual void SetName(std::string_view name) = 0;
  };

  class Texture : public Resource
  {
  public:
    D3D12_RESOURCE_DESC desc = {};
  };

  class Buffer : public Resource
  {
  public:
    uint64_t size = 0;
    GpuAddress gpu = 0;     // device address (vertex / index / constant reads)
    uint8_t *cpu = nullptr; // upload heap: mapped for the buffer's lifetime

    // Readback heap: maps [begin, end) for reading (the whole buffer is returned; read inside the range). Null on failure
    virtual const uint8_t *Map(size_t begin, size_t end) = 0;
    virtual void Unmap() = 0;
  };

  class Pipeline
  {
  public:
    virtual ~Pipeline() = default;
  };

  using TextureRef = std::shared_ptr<Texture>;
  using BufferRef = std::shared_ptr<Buffer>;
  using PipelineRef = std::shared_ptr<Pipeline>;

  // Shader bytecode (DXIL or SPIR-V, the device's ShaderFormat) and, for SPIR-V, the value of specialization constant 0
  struct ShaderCode
  {
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t spec = 0;

    explicit operator bool() const { return data && size; }
  };

  // A vertex input: D3D12's description plus the shader input location it feeds (Vulkan: the element's index in the vertex shader's input list, see sst_translate GetVertexInputs)
  struct InputElement
  {
    D3D12_INPUT_ELEMENT_DESC d3d = {};
    uint32_t location = 0;
  };

  // Root signature / pipeline layout
  enum class Layout : uint8_t
  {
    kDraw, // the translated shaders: VS / PS / shared constants + the bindless texture and sampler tables
    kBlit, // the renderer's own full-screen passes: 8 32-bit constants + one source SRV slot + a linear clamp sampler
  };

  struct PipelineDesc
  {
    Layout layout = Layout::kDraw;
    ShaderCode vs, ps; // ps empty: depth-only
    D3D12_BLEND_DESC blend = {};
    D3D12_RASTERIZER_DESC raster = {};
    D3D12_DEPTH_STENCIL_DESC depth = {};
    std::vector<InputElement> inputs;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN; // UNKNOWN: no colour target
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN; // UNKNOWN: no depth target
    uint32_t samples = 1;
  };

  // Pipelines kept between runs (D3D12: ID3D12PipelineLibrary; Vulkan: a VkPipelineCache)
  class PipelineCache
  {
  public:
    virtual ~PipelineCache() = default;

    // A pipeline stored under 'name' in an earlier run (null if absent or the driver can't use it)
    virtual PipelineRef Load(const std::wstring &name, const PipelineDesc &desc) = 0;

    // Creates the pipeline and stores it under 'name'. 'error': the backend's failure code (D3D12 HRESULT / VkResult)
    virtual PipelineRef Create(const std::wstring &name, const PipelineDesc &desc, int32_t *error) = 0;

    // Writes the cache file if it gained pipelines; returns the bytes written (0: nothing written)
    virtual size_t Save() = 0;
    virtual bool Dirty() const = 0;
    virtual size_t FileBytes() const = 0; // the file read at startup
  };

  // Timestamps: 'count' per pool, read back through a readback buffer
  class TimestampPool
  {
  public:
    virtual ~TimestampPool() = default;
  };

  using TimestampPoolRef = std::shared_ptr<TimestampPool>;

  // Sizes of the view tables (fixed at startup)
  struct TableSizes
  {
    uint32_t srv = 0, sampler = 0, rtv = 0, dsv = 0;
  };

  class CommandList
  {
  public:
    virtual ~CommandList() = default;

    // Starts recording into frame slot 'slot' (its memory must no longer be in use by the GPU) / finishes recording
    virtual void Begin(uint32_t slot, std::string_view name) = 0;
    virtual void End() = 0;

    // Transitions (the whole resource)
    virtual void Barrier(Texture &texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) = 0;
    virtual void Barrier(Buffer &buffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) = 0;

    // Clears (rects in the target's pixels; none = all of it)
    virtual void ClearRtv(uint32_t rtv, const float color[4], const D3D12_RECT *rects = nullptr, uint32_t rect_count = 0) = 0;
    virtual void ClearDsv(uint32_t dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil, const D3D12_RECT *rects = nullptr, uint32_t rect_count = 0) = 0;

    // Output targets (view table slots; kNoView = none)
    static constexpr uint32_t kNoView = UINT32_MAX;
    virtual void SetTargets(uint32_t rtv, uint32_t dsv) = 0;

    virtual void SetViewport(const D3D12_VIEWPORT &viewport) = 0;
    virtual void SetScissor(const D3D12_RECT &rect) = 0;
    virtual void SetBlendFactor(const float factor[4]) = 0;

    // Layout + its bindings. kDraw: the bindless tables are bound with it
    virtual void SetLayout(Layout layout) = 0;
    virtual void SetPipeline(Pipeline &pipeline) = 0;
    virtual void SetDrawConstants(GpuAddress vs, GpuAddress ps, GpuAddress shared) = 0;
    virtual void SetBlitConstants(const int32_t *values, uint32_t count) = 0; // up to 8
    virtual void SetBlitSource(uint32_t srv) = 0;

    virtual void SetVertexBuffer(uint32_t slot, GpuAddress address, uint32_t size, uint32_t stride) = 0;
    virtual void SetIndexBuffer(GpuAddress address, uint32_t size, DXGI_FORMAT format) = 0;
    virtual void SetTopology(D3D_PRIMITIVE_TOPOLOGY topology) = 0;
    virtual void Draw(uint32_t vertices, uint32_t first_vertex) = 0;
    virtual void DrawIndexed(uint32_t indices, uint32_t first_index, int32_t base_vertex) = 0;

    // Copies. Textures: placed footprints as from Device::Footprints (offset into 'src')
    virtual void CopyBufferToTexture(Texture &dst, uint32_t subresource, Buffer &src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) = 0;
    virtual void CopyBuffer(Buffer &dst, uint64_t dst_offset, Buffer &src, uint64_t src_offset, uint64_t size) = 0;
    virtual void CopyBufferAll(Buffer &dst, Buffer &src) = 0;

    // Timestamps
    virtual void WriteTimestamp(TimestampPool &pool, uint32_t index) = 0;
    virtual void ResolveTimestamps(TimestampPool &pool, uint32_t first, uint32_t count, Buffer &dst, uint64_t dst_offset) = 0;

    // Debug marker (RenderDoc / PIX)
    virtual void Marker(std::string_view text) = 0;
  };

  class Device
  {
  public:
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual ShaderFormat shader_format() const = 0;

    // Resources. Buffers: 'desc' is ignored, the size is 'size'
    virtual TextureRef CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE *clear = nullptr) = 0;
    virtual BufferRef CreateBuffer(uint64_t size, HeapType heap, D3D12_RESOURCE_STATES initial) = 0;

    // Video memory a texture of this description takes
    virtual uint64_t TextureBytes(const D3D12_RESOURCE_DESC &desc) = 0;

    // Upload layout of subresources [first, first + count) (offsets from 0), as D3D12's GetCopyableFootprints
    virtual void Footprints(const D3D12_RESOURCE_DESC &desc, uint32_t first, uint32_t count, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints, uint32_t *rows, uint64_t *row_bytes, uint64_t *total) = 0;

    // MSAA support for a render-target / depth format
    virtual bool SupportsSamples(DXGI_FORMAT format, uint32_t samples) = 0;

    // View tables
    virtual bool CreateTables(const TableSizes &sizes) = 0;
    virtual void WriteSrv(uint32_t slot, Texture &texture, const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) = 0;
    virtual void WriteSampler(uint32_t slot, const D3D12_SAMPLER_DESC &desc) = 0;
    virtual void WriteRtv(uint32_t slot, Texture &texture, const D3D12_RENDER_TARGET_VIEW_DESC *desc) = 0;
    virtual void WriteDsv(uint32_t slot, Texture &texture, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc) = 0;

    // Layouts (call once, after CreateTables)
    virtual bool CreateLayouts() = 0;

    // Pipelines (thread-safe)
    virtual PipelineRef CreatePipeline(const PipelineDesc &desc, int32_t *error = nullptr) = 0;
    virtual std::unique_ptr<PipelineCache> OpenPipelineCache(const std::filesystem::path &path) = 0;

    // Timestamps (null: unsupported)
    virtual TimestampPoolRef CreateTimestampPool(uint32_t count) = 0;
    virtual uint64_t TimestampFrequency() = 0;

    // Command lists: 'slots' frames of recording memory
    virtual std::unique_ptr<CommandList> CreateCommandList(uint32_t slots) = 0;

    // Runs a recorded (ended) list; returns the fence value that completes with it
    virtual uint64_t Submit(CommandList &list) = 0;
    virtual uint64_t CompletedFence() = 0;
    virtual void WaitFence(uint64_t value) = 0;
    virtual uint64_t LastSubmitted() const = 0;
  };
}