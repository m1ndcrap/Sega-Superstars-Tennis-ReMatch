// Native renderer: pipelines

#include "gpu/native/renderer_internal.h"
#include "gpu/native/pipeline_workers.h"

namespace sst::native
{
  using namespace renderer_detail;

  // pipelines
  uint64_t Renderer::DxilHash(const ShaderVariant *v)
  {
    if (!v)
      return 0;

    auto [it, fresh] = dxil_hashes_.try_emplace(v, 0);

    if (fresh)
      it->second = v->spec ? XXH3_64bits_withSeed(v->dxil.data(), v->dxil.size(), v->spec) : XXH3_64bits(v->dxil.data(), v->dxil.size()); // (SPIR-V variants share a module)

    return it->second;
  }

  void Renderer::FinishPipeline(uint64_t hash, PipelineJob &job, const PipelineKey &key)
  {
    if (job.state.load(std::memory_order_acquire) == 1)
    {
      ++stats_.pipelines_created;

      if (job.from_library)
        ++stats_.pipelines_from_cache;

      stats_.pipeline_ms += job.ms;
      pipelines_.emplace(hash, std::move(job.pso));
      return;
    }

    failed_pipelines_.insert(hash);
    ++stats_.pipelines_failed;

    if (stats_.pipelines_failed <= 10)
    {
      std::string inputs;

      for (const auto &e : job.desc.inputs)
        inputs += fmt::format(" {}{}:{}@{}+{}", e.d3d.SemanticName, e.d3d.SemanticIndex, int(e.d3d.Format), e.d3d.InputSlot, e.d3d.AlignedByteOffset);

      REXLOG_WARN("[sst-native] pipeline creation failed ({:08X}): blend {:08X} topology {} inputs{}", uint32_t(job.error), key.blend, key.topology_type, inputs);
    }
  }

  // urgent: a UI-like draw (no depth test, few vertices). Skipping it while its pipeline compiles makes menus and HUD blink out, and on slow CPUs (one background compile thread) a pipeline can wait behind a court's worth of others for seconds.
  gfx::Pipeline *Renderer::GetPipeline(const PipelineKey &key, const std::vector<gfx::InputElement> &layout, const ShaderVariant *vs, const ShaderVariant *ps, bool urgent)
  {
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));

    if (auto it = pipelines_.find(hash); it != pipelines_.end())
      return it->second.get();

    if (failed_pipelines_.count(hash))
      return nullptr;

    if (auto it = pending_pipelines_.find(hash); it != pending_pipelines_.end())
    {
      if (urgent && it->second->state.load(std::memory_order_acquire) == 0)
      {
        // Already queued for an earlier, non-urgent draw: wait for it (bounded)
        const auto t0 = std::chrono::steady_clock::now();

        while (it->second->state.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));

        ++stats_.pipelines_urgent;
        stats_.pipeline_urgent_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }

      if (it->second->state.load(std::memory_order_acquire) == 0)
      {
        ++stats_.skip_pipeline_pending;
        return nullptr;
      }

      FinishPipeline(hash, *it->second, key);
      pending_pipelines_.erase(it);
      auto done = pipelines_.find(hash);
      return done != pipelines_.end() ? done->second.get() : nullptr;
    }

    if (!pipeline_workers_)
    {
      const uint32_t threads = std::clamp(uint32_t(std::thread::hardware_concurrency()) / 4, 1u, 4u);
      pipeline_workers_ = std::make_unique<PipelineWorkers>(*gfx_, threads, REXCVAR_GET(sst_native_pipeline_library), shaders_ && shaders_->fast());
    }

    gfx::PipelineDesc d;
    d.layout = gfx::Layout::kDraw;
    d.vs = {vs->dxil.data(), vs->dxil.size(), vs->spec};

    if (ps)
      d.ps = {ps->dxil.data(), ps->dxil.size(), ps->spec}; // null: depth-only

    const uint32_t b = key.blend;
    D3D12_RENDER_TARGET_BLEND_DESC &rt = d.blend.RenderTarget[0];
    rt.SrcBlend = BlendFactor(b & 0x1F, false);
    rt.BlendOp = BlendOp((b >> 5) & 7);
    rt.DestBlend = BlendFactor((b >> 8) & 0x1F, false);
    rt.SrcBlendAlpha = BlendFactor((b >> 16) & 0x1F, true);
    rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
    rt.DestBlendAlpha = BlendFactor((b >> 24) & 0x1F, true);
    rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO && rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE && rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = key.color_mask;

    d.raster.FillMode = D3D12_FILL_MODE_SOLID;
    d.raster.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK
                                                                              : D3D12_CULL_MODE_NONE;
    d.raster.FrontCounterClockwise = key.front_ccw;
    d.raster.DepthClipEnable = TRUE;
    d.raster.DepthBias = key.depth_bias;
    d.raster.SlopeScaledDepthBias = key.slope_bias;

    d.depth.DepthEnable = key.depth_enable;
    d.depth.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.depth.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func + 1); // xenos 0-7

    d.inputs = layout;
    d.strip_cut = key.strip_cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                                                      : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

    d.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
    d.rtv = key.rtv_format ? DXGI_FORMAT(key.rtv_format) : kOutputFormat;
    const uint32_t dsv_kind = key.dsv & 0xF;
    d.dsv = dsv_kind == 0 ? kDepthFormat : dsv_kind == 2 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                                         : DXGI_FORMAT_UNKNOWN;

    d.samples = 1u << (key.dsv >> 4);

    auto job = std::make_shared<PipelineJob>();
    job->hash = hash;
    job->desc = std::move(d);

    // Library name: everything in the key, with the shaders by content (the variant pointers differ between runs)
    {
      PipelineKey stable = key;
      stable.vs = nullptr;
      stable.ps = nullptr;
      const uint64_t parts[3] = {XXH3_64bits(&stable, sizeof(stable)), DxilHash(vs), DxilHash(ps)};
      const std::string name = fmt::format("sst{:016X}", XXH3_64bits(parts, sizeof(parts)));
      job->name.assign(name.begin(), name.end());
    }

    if (REXCVAR_GET(sst_native_pipeline_async) && !urgent)
    {
      if (pipeline_workers_->TryLoad(*job))
      {
        FinishPipeline(hash, *job, key);
        auto done = pipelines_.find(hash);
        return done != pipelines_.end() ? done->second.get() : nullptr;
      }

      pending_pipelines_.emplace(hash, job);
      pipeline_workers_->Submit(job);
      ++stats_.skip_pipeline_pending;
      return nullptr;
    }

    pipeline_workers_->Build(*job);

    if (urgent)
    {
      ++stats_.pipelines_urgent;
      stats_.pipeline_urgent_ms += job->ms;
    }

    FinishPipeline(hash, *job, key);
    auto done = pipelines_.find(hash);
    return done != pipelines_.end() ? done->second.get() : nullptr;
  }
}