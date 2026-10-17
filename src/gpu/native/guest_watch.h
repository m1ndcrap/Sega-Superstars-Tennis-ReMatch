// Native renderer: what the renderer has seen the game rewrite (textures, vertex buffers) and which constants each shader reads, for the game thread's capture (capture*.cpp)

#pragma once

#include <cstdint>
#include <unordered_map>

namespace sst::native
{
  // Guest data the game rewrites while the game runs. With the render thread a frame behind, the game may already have written the next frame's data when the renderer reads it, so the game thread copies it when it captures a draw (native.cpp) and the renderer reads the copies. The render thread records what it has seen; the game thread reads a copy of the set when it changes.
  // Textures: every texture the renderer knows gets flagged `changing` once its data changed (unflagged a minute later). The game thread copies changing textures for every draw, and all known textures for UI-sized draws (where a one-frame mismatch is most visible: portraits, court previews, text pages reloaded as screens change).
  // Vertex buffers: start addresses whose data changed between frames, with the bytes draws read from them
  namespace guest_watch
  {
    struct TextureRange
    {
      uint32_t base_addr = 0, base_size = 0; // physical
      uint32_t mip_addr = 0, mip_size = 0;
      bool changing = false;
      bool external = false; // a render-target resolve destination: the renderer samples its own copy, never guest memory
    };

    struct Set
    {
      std::unordered_map<uint32_t, TextureRange> textures; // by base page (physical address >> 12)
      std::unordered_map<uint32_t, uint32_t> vertices;     // physical start -> bytes read
    };

    void KnownTexture(uint32_t base_page, const TextureRange &range);    // render thread: texture created
    void TextureChanged(uint32_t base_page, uint64_t frame);             // render thread: its data changed
    void ExternalTexture(uint32_t base_page, bool external);             // render thread: became / stopped being a resolve destination
    void VerticesChanged(uint32_t phys, uint32_t bytes, uint64_t frame); // render thread: vertex data changed between frames
    void Prune(uint64_t frame);
    uint64_t Version(); // changes whenever the set does
    void Copy(Set &out);

    // Float constant chunks a shader's draws read: bit i = c[16i ... 16i+15] (Renderer::ConstantChunkMask; memory-export shaders and shaders with relative constant addressing: all 16), keyed by shader object and microcode pointer (ShaderKey). The game thread compares only those constant chunks of the device block per draw. Only ever grows per key (bits are ORed in)
    inline uint64_t ShaderKey(uint32_t object, uint32_t ucode) { return (uint64_t(object) << 32) | ucode; }
    void ConstantUse(uint64_t key, uint32_t chunk_mask); // render thread
    uint64_t ConstantUseVersion();
    void CopyConstantUse(std::unordered_map<uint64_t, uint32_t> &out);
  }
}