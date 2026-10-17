// The SumoTool scene format (a scene's .DAT, read into a model and written back; its .REL relocations rebuilt to match). Shared by the scene patchers (scene_widen.cpp, scene_rows.cpp, scene_achievements.cpp; pause_patch.cpp uses the chunk layer).
//
// A .DAT is a list of chunks (16-byte header: tag, chunk size, data size, magic). The ones the patchers change:
//   KFRM  keyframe tracks, one per object (position, scale, colour keys)
//   OBJS  objects: groups (GROP: children), pictures (TXTR), texts (TEXT), etc
//   PTEX  texture entries (uv rectangles on texture pages) and the pages' headers
//   SCNE  scene pages, each listing its top-level objects
//
// The .REL lists, per chunk (RELO), the offsets of the pointer fields the loader fixes up.

#pragma once

#include "menu/be_bytes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sst::menu::scene
{
  struct Chunk
  {
    uint32_t tag, magic;
    Bytes data;
  };

  struct Track
  {
    uint16_t count = 0, kind = 0;
    float w = 0, h = 0;
    Bytes keys;
    uint32_t order = 0; // position of the keys in the file (new tracks go last)

    // Keyframe sizes: kind 0 0x28, kind 2 0x38, kind 1 (particles) 0x64
    size_t KeySize() const { return kind == 2 ? 0x38 : kind == 1 ? 0x64
                                                                 : 0x28; }

    uint8_t *Key(int k) { return keys.data() + size_t(k) * KeySize(); }
    float X(int k) const { return RdF(keys.data() + size_t(k) * KeySize() + 0xC); }
    float Sx(int k) const { return RdF(keys.data() + size_t(k) * KeySize() + 0x14); }
  };

  struct Object
  {
    uint32_t type = 0;
    Bytes blob;                     // GROP: header only (0x1C bytes)
    std::vector<uint32_t> children; // GROP
    uint32_t order = 0;             // position in the file (new objects go last)
  };

  struct Ptex
  {
    float uv[8]; // TL, TR, BR, BL
    uint32_t page;
  };

  struct SceneEntry
  {
    uint32_t hash, list_at, zero; // list_at: where the list was (lists keep their order)
    float w, h;
    std::vector<uint32_t> list;
  };

  struct Scene
  {
    std::vector<Chunk> chunks;
    std::map<uint32_t, Track> tracks;
    std::map<uint32_t, Object> objects;
    std::map<uint32_t, Ptex> ptex;

    // Texture pages: data offset in the .GPU file and the 0x34-byte header (D3D resource header, then the fetch constant at +0x1C)
    struct Page
    {
      uint32_t data_offset;
      Bytes header;
    };

    std::vector<Page> pages;
    std::vector<SceneEntry> scenes;
    std::map<uint32_t, uint32_t> parent; // object -> its group
    uint32_t next_order = 0x7FFF0000;
    std::set<uint32_t> used;

    Chunk *Find(uint32_t tag)
    {
      for (Chunk &c : chunks)
      {
        if (c.tag == tag)
          return &c;
      }

      return nullptr;
    }

    uint32_t NewHash(uint32_t seed)
    {
      uint32_t h = seed ^ 0x57494445u; // "WIDE"

      do
      {
        h = h * 0x9E3779B1u + 0x7F4A7C15u;
        h ^= h >> 15;
      } while (!h || used.count(h));

      used.insert(h);
      return h;
    }
  };

  // Chunk tag -> the offsets of its pointer fields (for the .REL)
  using Relocs = std::vector<std::pair<uint32_t, std::vector<uint32_t>>>;

  // Chunks
  std::optional<std::vector<Chunk>> ReadChunks(const uint8_t *data, size_t size, std::string &error);
  Bytes WriteChunks(const std::vector<Chunk> &chunks);
  Chunk *FindChunk(std::vector<Chunk> &chunks, uint32_t tag);

  // Splits a chunk's blob area into per-entry blobs (each runs to the next offset or the end of the chunk)
  std::map<uint32_t, Bytes> SplitBlobs(const Bytes &data, const std::vector<uint32_t> &offsets, size_t start, bool &ok);

  // model
  bool Parse(const uint8_t *data, size_t size, Scene &s, std::string &error);

  // Rebuilds the chunks; `relocs` gets the pointer fields per chunk
  void Write(Scene &s, Relocs &relocs);

  // The .REL with its RELO chunks rebuilt from `relocs`
  bool PatchRel(const uint8_t *data, size_t size, const Relocs &relocs, Bytes &out, std::string &error);

  // Write + PatchRel: the scene's new files
  bool Finish(Scene &s, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Adds `child` next to `after` (in its group, or in the scene lists that hold it)
  void InsertAfter(Scene &s, uint32_t after, uint32_t child);
}