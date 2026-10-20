// The working state of one draw (Renderer::Draw's steps, renderer_draw.cpp and renderer_debug.cpp)

#pragma once

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  // What one draw works out on its way to the command list
  struct Renderer::DrawContext
  {
    DrawContext(const uint8_t *base, const DrawCall &call, Profile &prof) : base(base), call(call), dev(call.device), clk(prof, kPhOther) {}

    const uint8_t *base;
    const DrawCall &call;
    const uint32_t dev; // guest D3D device: its state block holds the registers, constants and bound objects
    PhaseClock clk;

    // Shaders: guest objects, cache entries, compiled variants
    uint32_t vs_obj = 0, ps_obj = 0;
    ShaderEntry *vs_entry = nullptr, *ps_entry = nullptr;
    const ShaderVariant *vs = nullptr, *ps = nullptr;

    // Render target: the guest colour surface and whether it gets its own host texture
    uint32_t rt0 = 0;
    bool rt_mode = false;

    // Primitive type: host topology; TRIFAN / QUADLIST are converted to triangle lists, RECTLIST gets its 4th corners on the CPU
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    bool rect_list = false, convert = false;

    // Vertex input layout: the guest streams (fetch constant + stride) and one host element (4 x 32 bits) per shader input
    struct FetchStream
    {
      uint32_t slot;   // fetch constant slot (2-dword units)
      uint32_t stride; // bytes
    };

    struct HostElement
    {
      FetchElement fetch;
      int stream;          // -1: reads zeros
      uint32_t fetch_addr; // instruction index of the controlling full vfetch
    };

    FetchStream fetch_streams[kMaxStreams];
    uint32_t fetch_stream_count = 0;
    std::vector<HostElement> host_elements;
    std::vector<gfx::InputElement> layout;
    static constexpr size_t kLayoutHashWords = 64;
    uint64_t layout_hash_input[kLayoutHashWords]; // what the layout is made of (hashed into the pipeline key)
    uint32_t layout_words = 0;
    bool computed_fetch = false; // element indices come from running the VS on the CPU
    int position_element = -1;   // host element holding POSITION (RECTLIST corner detection)
    const uint32_t vs_spec = 0;  // R11G11B10 normals are decoded on the CPU
    std::string capture_detail;  // F9 capture: inputs of this draw

    // Render state and targets
    uint32_t depthcontrol = 0, colorcontrol = 0, mode = 0;
    HostRT *color_rt = nullptr, *depth_rt = nullptr;
    uint32_t target_w = 0, target_h = 0;
    bool has_depth = false;
    Scale2 draw_scale;
    bool debug_shading = false;
    bool bisect_hide = false;
    PipelineKey key;

    // Indices, decoded with the index buffer's own swap mode; the referenced vertex range
    std::vector<uint32_t> indices; // little-endian, as the GPU reads them
    bool index32 = false;
    uint32_t vmin = 0, vmax = 0;
    const uint8_t *ib_src = nullptr; // guest index data (late-write check)
    size_t ib_src_bytes = 0;
    GeoEntry *ientry = nullptr; // geometry cache entry of the index data
    uint64_t ihash = 0;
    bool index_hit = false;

    // Vertex data: each stream's guest (or game-thread copy) source
    struct ResolvedStream
    {
      const uint8_t *src;
      uint32_t endian;
      uint32_t size;     // bytes
      uint32_t phys = 0; // guest physical address (vertex buffers)
    };

    uint32_t first_vertex = 0, vertex_count = 0;
    ResolvedStream resolved[kMaxStreams] = {};
    bool reads_morph = false;
    std::vector<uint32_t> rect_indices; // RECTLIST (4 vertices per rectangle)

    // What gets recorded
    gfx::Pipeline *pso = nullptr;
    uint32_t draw_count = 0;
    bool draw_indexed = false;
    gfx::GpuAddress vsc_gpu = 0, psc_gpu = 0, shared_gpu = 0;
    float half_x = 0, half_y = 0; // half-pixel offset (F9 capture)
  };
}