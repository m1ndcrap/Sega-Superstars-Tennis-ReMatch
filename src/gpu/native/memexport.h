// sst_rematch - native renderer: CPU execution of memory-export vertex shaders

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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

  // A memory-export run handed to the background thread (MemexportRunner::Submit): everything the shader reads is copied in, so the render thread can go on recording draws while it runs.
  struct MemexportJob
  {
    std::vector<uint32_t> ucode;
    std::vector<float> constants; // 256 x float4
    uint32_t bools[8] = {}, loops[32] = {}, fetch[192] = {};
    std::vector<uint32_t> indices;

    // Cache: with `use_cache`, the job hashes everything the shader reads (microcode, the constants it uses, indices, fetch constants and input buffers) and skips the run when that and the output range [hash_begin, +hash_size) still match the previous run's hashes (`previous_*`).
    bool use_cache = false, has_previous = false;
    uint64_t previous_input_hash = 0, previous_output_hash = 0;
    uint32_t hash_begin = 0, hash_size = 0;

    // Results, valid once the job's ticket has finished.
    bool ok = false, skipped = false;
    MemexportStats stats;
    uint64_t input_hash = 0, output_hash = 0;
    double ms = 0;
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
    bool Run(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, MemexportStats *stats, bool force_per_vertex = false);

    // Debug: runs an ordinary vertex shader on the CPU and returns each vertex's position export (clip space xyzw, 4 floats per vertex; NaN if the shader didn't export one). Memory exports, if any, are still written.
    bool RunPositions(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, float *positions, MemexportStats *stats);

    // For vertex shaders whose fetches don't use the plain vertex index (e.g. particle quads fetching one record per 4 vertices): runs the shader on the CPU and returns, per full vfetch instruction address, the element index each vertex fetched (-1 if that fetch didn't run for it).
    bool RunFetchIndices(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, std::unordered_map<uint32_t, std::vector<int32_t>> *records);

    // Runs a job on the calling thread (cache check, run, output hash).
    void Execute(MemexportJob &job);

    // Background queue (first in, first out). Returns the job's ticket.
    uint64_t Submit(std::shared_ptr<MemexportJob> job);

    // Waits until every job up to and including `ticket` has finished.
    void Wait(uint64_t ticket);

    // highest finished ticket
    uint64_t finished() const;

  private:
    struct Pool;
    struct Queue;
    uint8_t *membase_;
    std::unique_ptr<Pool> pool_;
    std::unique_ptr<Queue> queue_;
  };

  // Float constant registers the shader's ALU instructions read (bit per register in used[4]); false if it uses relative constant addressing (then any register may be read)
  bool FloatConstantsRead(const uint32_t *ucode, uint32_t dwords, uint64_t used[4]);

  // Fetch constant slots (0-95) the shader's vertex fetches read
  uint32_t VertexFetchSlots(const uint32_t *ucode, uint32_t dwords, uint32_t *slots, uint32_t max);

  // Texture fetch constants (0-31) the shader's texture fetches read, with the first fetch's dimension (xenos::FetchOpDimension + 1; 0 = unused) and filter overrides (xenos::TextureFilter / AnisoFilter values; kUseFetchConst = 3 / 7 when the fetch constant decides).
  struct TextureFetchUse
  {
    uint8_t dimension = 0;
    uint8_t mag_filter = 3, min_filter = 3, mip_filter = 3, aniso_filter = 7;
    bool unnormalized = false, mixed_dimensions = false;
    uint16_t count = 0;
    float lod_bias = 0;
  };

  uint32_t TextureFetchUsage(const uint32_t *ucode, uint32_t dwords, TextureFetchUse uses[32]);

  // Element size in bytes of a memexport stream format (xenos::ColorFormat), 0 if unsupported
  uint32_t MemexportElementSize(uint32_t format);

  // Disassembly-lite of the control flow / instructions (for logs)
  std::string DescribeMicrocode(const uint32_t *ucode, uint32_t dwords);
}