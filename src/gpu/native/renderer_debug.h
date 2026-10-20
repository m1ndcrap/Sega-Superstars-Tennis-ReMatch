// Native renderer: debug tools shared between the draw path and renderer_debug.cpp

#pragma once

#include "gpu/native/renderer_internal.h"

namespace sst::native::renderer_detail
{
  // --sst_native_debug_bisect (render thread only): halves the candidate pixel shaders on each key press
  struct ShaderBisect
  {
    std::unordered_set<uint64_t> recent; // pixel shaders drawn since the last start
    std::vector<uint64_t> candidates;    // sorted; the first `hidden` are skipped
    size_t hidden = 0;
    bool active = false;

    bool Hidden(uint64_t ps) const
    {
      return active && std::binary_search(candidates.begin(), candidates.begin() + ptrdiff_t(hidden), ps);
    }

    void Log(const char *what) const
    {
      std::string list;

      for (size_t i = 0; i < candidates.size() && i < 64; ++i)
        list += fmt::format(" {:016X}{}", candidates[i], i < hidden ? "*" : "");

      REXLOG_INFO("[sst-bisect] {}: {} candidate pixel shaders, {} hidden (*):{}", what, candidates.size(), hidden, list);

      if (candidates.size() == 1)
        REXLOG_INFO("[sst-bisect] FOUND: pixel shader {:016X}", candidates[0]);
    }

    void Split() { hidden = candidates.size() > 1 ? candidates.size() / 2 : candidates.size(); }

    void Poll()
    {
      if (!REXCVAR_GET(sst_native_debug_bisect))
        return;

      if (DebugKeyPressed(0x24)) // Home
      {
        candidates.assign(recent.begin(), recent.end());
        std::sort(candidates.begin(), candidates.end());
        active = !candidates.empty();
        Split();
        Log("start");
      }
      else if (active && DebugKeyPressed(0x21)) // Page Up
      { // still visible: the culprit is among the shown ones
        candidates.erase(candidates.begin(), candidates.begin() + ptrdiff_t(hidden));
        Split();
        Log("still visible");
      }
      else if (active && DebugKeyPressed(0x22)) // Page Down
      { // gone: the culprit is among the hidden ones
        candidates.resize(hidden);
        Split();
        Log("gone");
      }
      else if (DebugKeyPressed(0x23)) // End
      {
        active = false;
        recent.clear();
        REXLOG_INFO("[sst-bisect] stopped");
      }
    }
  };

  // The one bisect session (render thread)
  ShaderBisect &Bisect();
}