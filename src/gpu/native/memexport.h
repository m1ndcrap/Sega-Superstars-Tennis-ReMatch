// sst_rematch - native renderer: CPU execution of memory-export vertex shaders

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sst::native
{

  // Vertex shader state the interpreter reads (all host-endian)
  struct VsConstants
  {
    const float *float_constants = nullptr;    // 256 x float4
    const uint32_t *bool_constants = nullptr;  // 8 dwords (VS bools 0-127, PS 128-255)
    const uint32_t *loop_constants = nullptr;  // 32 dwords, may be null (= all zero)
    const uint32_t *fetch_constants = nullptr; // 96 slots x 2 dwords (vertex fetch layout)
  };

  struct MemexportStats
  {
    uint64_t vertices = 0;         // shader invocations
    uint64_t elements = 0;         // eM# values written
    uint64_t bad_address = 0;      // eA without the stream marker bits
    uint64_t bad_format = 0;       // unsupported export format
    uint64_t out_of_range = 0;     // outside guest physical memory
    uint64_t fetch_oob = 0;        // vertex fetch outside its buffer
    uint64_t malformed = 0;        // control flow limit hit / bad addresses
    uint64_t per_vertex_draws = 0; // shaders run one vertex at a time (per-vertex flow)
    uint32_t last_format = 0;      // format of the last flushed export
    uint32_t last_base = 0;        // physical byte address of the last stream
    uint32_t last_count = 0;       // element count of the last stream
  };

  class MemexportRunner
  {
  public:
    // physical_membase: host address of guest physical memory (512 MB)
    // worker_threads: extra threads for large draws (0 = run on the caller only)

    MemexportRunner(uint8_t *physical_membase, uint32_t worker_threads);
    ~MemexportRunner();
    uint32_t worker_threads() const;

    // ucode: host-endian microcode (control flow first), `dwords` long
    // Runs the shader once per entry of `vertex_indices` (r0.x = index), writing all memory exports. Returns false if nothing could be run
    bool Run(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants,
             const uint32_t *vertex_indices, uint32_t count, MemexportStats *stats,
             bool force_per_vertex = false);

  private:
    struct Pool;
    uint8_t *membase_;
    std::unique_ptr<Pool> pool_;
  };

  // Float constant registers the shader's ALU instructions read (bit per register in used[4]); false if it uses relative constant addressing (then any register may be read).
  bool FloatConstantsRead(const uint32_t *ucode, uint32_t dwords, uint64_t used[4]);

  // Fetch constant slots (0-95) the shader's vertex fetches read
  uint32_t VertexFetchSlots(const uint32_t *ucode, uint32_t dwords, uint32_t *slots, uint32_t max);

  // Element size in bytes of a memexport stream format (xenos::ColorFormat), 0 if unsupported
  uint32_t MemexportElementSize(uint32_t format);

  // Disassembly-lite of the control flow / instructions (for logs)
  std::string DescribeMicrocode(const uint32_t *ucode, uint32_t dwords);

}