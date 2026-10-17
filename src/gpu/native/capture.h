// Captures one hooked D3D call (game threads). A Capture holds the capture lock for its lifetime, builds one Command at a time and queues it for the render thread with the copies it needs:
// - the device block delta and the bound vertex shader's live microcode (every draw, clear and resolve) (gets from capture.cpp)
// - textures the game rewrites (gets from capture_textures.cpp)
// - vertex buffers the game rewrites (gets from capture_vertices.cpp)

#pragma once

#include "gpu/native/capture_state.h"

namespace sst::native::capture
{
  class Capture
  {
  public:
    explicit Capture(State &s);
    ~Capture();

    // The command being built (replaces any unsubmitted one)
    Command &New(Command::Kind kind);

    void Submit();

    // Ends the frame: hands its arena over and waits until the render thread is at most one frame behind, with an arena free
    void EndFrame();

    // Copies guest [addr, addr + size) into the frame's arena and overlays it for the command
    void Range(uint32_t addr, uint32_t size);

    // A copy of host bytes in the frame's arena
    const uint8_t *Bytes(const void *data, size_t size);

    // The device block delta and the bound vertex shader's live microcode
    void Device(uint32_t dev);

    // Whether the bound state tests depth (anything but ALWAYS): part of the 3D scene, not UI, however few vertices the draw has
    bool DepthTested(uint32_t dev) const;

    // Vertices a draw reads from the start of each vertex buffer (start + count, or the highest index + base vertex); 0 when too big to check cheaply
    uint32_t DrawVertices(uint32_t dev, const DrawCall &call) const;

    // Textures the draw samples that the game rewrites. ui: a UI-sized draw (copies any known texture, within a frame budget)
    void Textures(uint32_t dev, bool ui);

    // Vertex buffers the draw reads that the game rewrites. vertices: DrawVertices for UI draws (0: unknown)
    void Vertices(uint32_t dev, uint32_t vertices, bool ui, const DrawCall &call);

    // Times one part of a capture (perf line "capture split")
    struct Timer
    {
      uint64_t &ticks;
      uint64_t t0 = Ticks();
      ~Timer() { ticks += Ticks() - t0; }
    };

    Timer Time(CapturePerf::Part part) { return Timer{s_.perf.part_ticks[part]}; }

  private:
    void Overlay(uint32_t addr, uint32_t size, const uint8_t *data);
    void Flush();
    uint64_t ConstantChunks(uint32_t dev);
    void DeviceDelta(uint32_t dev, uint64_t compare);
    void Microcode(uint32_t vs);
    void RefreshWatched();

    // capture_textures.cpp
    struct TextureList;
    uint64_t HashTexture(const guest_watch::TextureRange &r);
    void CopyTextureRange(uint32_t phys, uint32_t size, TextureList &out);

    // capture_vertices.cpp
    struct VertexList;
    bool CopyWatchedVertices(uint32_t phys, uint32_t size, uint32_t watched_size, VertexList &out);
    void CopyVertices(uint32_t phys, uint32_t size, bool ui, uint32_t watched_size, VertexList &out);
    uint32_t Be32(uint32_t addr) const { return capture::Be32(s_.base, addr); }

    State &s_;
    std::lock_guard<std::mutex> lock_;
    uint64_t start_; // (Ticks)
    Command cmd_;
  };
}