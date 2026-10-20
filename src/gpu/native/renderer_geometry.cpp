// Native renderer: the geometry cache (decoded vertex / index data kept on the GPU between frames)

#include "gpu/native/draw_context.h"
#include "gpu/native/renderer_debug.h"

namespace sst::native
{
  using namespace renderer_detail;

  bool Renderer::GeoCacheUsable()
  {
    if (REXCVAR_GET(sst_native_geometry_cache_mb) <= 0 || capture_active_ || REXCVAR_GET(sst_native_check_vs) || REXCVAR_GET(sst_native_check_vs_all) || REXCVAR_GET(sst_native_check_late_writes))
      return false;

    if (!geo_pool_ && !geo_failed_)
    {
      geo_pool_size_ = uint64_t(std::min(REXCVAR_GET(sst_native_geometry_cache_mb), 4000)) << 20;
      geo_pool_ = gfx_->CreateBuffer(geo_pool_size_, gfx::HeapType::kDefault, D3D12_RESOURCE_STATE_COMMON);

      if (!geo_pool_)
      {
        geo_failed_ = true;
        REXLOG_WARN("[sst-native] geometry cache: could not allocate {} MB", geo_pool_size_ >> 20);
        return false;
      }

      geo_pool_->SetName("sst geometry cache");

      geo_state_ = D3D12_RESOURCE_STATE_COMMON;
      REXLOG_INFO("[sst-native] geometry cache: {} MB of video memory", geo_pool_size_ >> 20);
    }

    if (!geo_pool_)
      return false;

    // Buffers decay to COMMON after each submission: make it readable as vertex / index data for this frame's draws
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);

    if (geo_state_ != kRead)
    {
      list_->Barrier(*geo_pool_, geo_state_, kRead);
      geo_state_ = kRead;
    }

    return true;
  }

  gfx::GpuAddress Renderer::GeoAddress(const GeoEntry &e) const
  {
    return geo_pool_->gpu + e.offset;
  }

  void Renderer::GeoStore(GeoEntry &e, const Upload &src, uint32_t size)
  {
    if (!geo_pool_ || !src.buffer || !size)
      return;

    const uint64_t aligned = (uint64_t(size) + 255) & ~uint64_t(255);

    if (aligned > geo_pool_size_ / 4)
      return; // not worth a quarter of the pool

    if (geo_offset_ + aligned > geo_pool_size_)
    {
      // Full: start over. Older entries become misses (generation); copies later in this frame are ordered after the draws that read the old data
      ++geo_generation_;
      geo_offset_ = 0;
      ++stats_.geo_flushes;
    }

    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
    list_->Barrier(*geo_pool_, geo_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyBuffer(*geo_pool_, geo_offset_, *src.buffer, src.offset, size);
    list_->Barrier(*geo_pool_, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    geo_state_ = kRead;
    e.offset = geo_offset_;
    e.size = size;
    e.generation = geo_generation_;
    geo_offset_ += aligned;
    ++stats_.geo_stores;
    stats_.geo_store_bytes += size;
  }
}