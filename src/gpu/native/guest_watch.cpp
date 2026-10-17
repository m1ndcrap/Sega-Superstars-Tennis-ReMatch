// Native renderer: guest data the game rewrites

#include "gpu/native/guest_watch.h"

#include <atomic>
#include <mutex>

namespace sst::native
{
  namespace guest_watch
  {
    namespace
    {
      constexpr uint64_t kForgetFrames = 3600; // a minute without changes

      struct TextureEntry
      {
        TextureRange range;
        uint64_t last_change = 0;
      };

      struct VertexEntry
      {
        uint32_t bytes = 0;
        uint64_t last_change = 0;
      };

      std::mutex g_mutex;
      std::unordered_map<uint32_t, TextureEntry> g_textures;
      std::unordered_map<uint32_t, VertexEntry> g_vertices;
      std::atomic<uint64_t> g_version{0};

      void Bump() { g_version.fetch_add(1, std::memory_order_release); }
    }

    void KnownTexture(uint32_t base_page, const TextureRange &range)
    {
      std::lock_guard lock(g_mutex);
      TextureEntry &e = g_textures[base_page];
      const bool changing = e.range.changing, external = e.range.external;
      e.range = range;
      e.range.changing = changing;
      e.range.external = external;
      Bump();
    }

    void ExternalTexture(uint32_t base_page, bool external)
    {
      std::lock_guard lock(g_mutex);
      TextureEntry &e = g_textures[base_page]; // (also before the texture is ever loaded from guest memory)

      if (e.range.external != external)
      {
        e.range.external = external;
        Bump();
      }
    }

    void TextureChanged(uint32_t base_page, uint64_t frame)
    {
      std::lock_guard lock(g_mutex);
      auto it = g_textures.find(base_page);

      if (it == g_textures.end())
        return;

      it->second.last_change = frame;

      if (!it->second.range.changing)
      {
        it->second.range.changing = true;
        Bump();
      }
    }

    void VerticesChanged(uint32_t phys, uint32_t bytes, uint64_t frame)
    {
      std::lock_guard lock(g_mutex);
      VertexEntry &e = g_vertices[phys];
      e.last_change = frame;

      if (bytes > e.bytes)
      {
        e.bytes = bytes;
        Bump();
      }
    }

    void Prune(uint64_t frame)
    {
      std::lock_guard lock(g_mutex);
      bool changed = false;

      for (auto &[page, e] : g_textures)
        if (e.range.changing && frame > e.last_change + kForgetFrames)
        {
          e.range.changing = false;
          changed = true;
        }

      changed |= std::erase_if(g_vertices, [&](const auto &e)
                               { return frame > e.second.last_change + kForgetFrames; }) > 0;

      if (changed)
        Bump();
    }

    uint64_t Version() { return g_version.load(std::memory_order_acquire); }

    namespace
    {
      std::mutex g_constant_mutex;
      std::unordered_map<uint64_t, uint32_t> g_constant_use;
      std::atomic<uint64_t> g_constant_version{0};
    }

    void ConstantUse(uint64_t key, uint32_t chunk_mask)
    {
      std::lock_guard lock(g_constant_mutex);
      uint32_t &m = g_constant_use[key];

      if ((m | chunk_mask) != m)
      {
        m |= chunk_mask;
        g_constant_version.fetch_add(1, std::memory_order_release);
      }
    }

    uint64_t ConstantUseVersion() { return g_constant_version.load(std::memory_order_acquire); }

    void CopyConstantUse(std::unordered_map<uint64_t, uint32_t> &out)
    {
      std::lock_guard lock(g_constant_mutex);
      out = g_constant_use;
    }

    void Copy(Set &out)
    {
      std::lock_guard lock(g_mutex);
      out.textures.clear();
      out.vertices.clear();

      for (const auto &[page, e] : g_textures)
        out.textures.emplace(page, e.range);

      for (const auto &[phys, e] : g_vertices)
        out.vertices.emplace(phys, e.bytes);
    }
  }
}