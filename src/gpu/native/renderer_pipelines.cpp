// Native renderer: pipeline state objects

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
      it->second = XXH3_64bits(v->dxil.data(), v->dxil.size());

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

      for (const auto &e : job.layout)
        inputs += fmt::format(" {}{}:{}@{}+{}", e.SemanticName, e.SemanticIndex, int(e.Format), e.InputSlot, e.AlignedByteOffset);

      REXLOG_WARN("[sst-native] pipeline creation failed ({:08X}): blend {:08X} topology {} inputs{}", uint32_t(job.hr), key.blend, key.topology_type, inputs);
    }
  }

  // urgent: a UI-like draw (no depth test, few vertices). Skipping it while its pipeline compiles makes menus and HUD blink out, and on slow CPUs (one background compile thread) a pipeline can wait behind a court's worth of others for seconds.
  ID3D12PipelineState *Renderer::GetPipeline(const PipelineKey &key, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const ShaderVariant *vs, const ShaderVariant *ps, bool urgent)
  {
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));

    if (auto it = pipelines_.find(hash); it != pipelines_.end())
      return it->second.Get();

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
      return done != pipelines_.end() ? done->second.Get() : nullptr;
    }

    if (!pipeline_workers_)
    {
      const uint32_t threads = std::clamp(uint32_t(std::thread::hardware_concurrency()) / 4, 1u, 4u);
      pipeline_workers_ = std::make_unique<PipelineWorkers>(device_, threads, REXCVAR_GET(sst_native_pipeline_library), shaders_ && shaders_->fast());
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root_signature_.Get();
    d.VS = {vs->dxil.data(), vs->dxil.size()};

    if (ps)
      d.PS = {ps->dxil.data(), ps->dxil.size()}; // null: depth-only

    const uint32_t b = key.blend;
    D3D12_RENDER_TARGET_BLEND_DESC &rt = d.BlendState.RenderTarget[0];
    rt.SrcBlend = BlendFactor(b & 0x1F, false);
    rt.BlendOp = BlendOp((b >> 5) & 7);
    rt.DestBlend = BlendFactor((b >> 8) & 0x1F, false);
    rt.SrcBlendAlpha = BlendFactor((b >> 16) & 0x1F, true);
    rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
    rt.DestBlendAlpha = BlendFactor((b >> 24) & 0x1F, true);
    rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO && rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE && rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = key.color_mask;
    d.SampleMask = UINT_MAX;

    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK
                                                                                       : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = key.front_ccw;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.RasterizerState.DepthBias = key.depth_bias;
    d.RasterizerState.SlopeScaledDepthBias = key.slope_bias;

    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func + 1); // xenos 0-7

    d.InputLayout = {layout.data(), UINT(layout.size())};
    d.IBStripCutValue = key.strip_cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                                                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = key.rtv_format ? DXGI_FORMAT(key.rtv_format) : D3D12Presenter::kGuestOutputFormat;
    const uint32_t dsv_kind = key.dsv & 0xF;
    d.DSVFormat = dsv_kind == 0 ? kDepthFormat : dsv_kind == 2 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                                               : DXGI_FORMAT_UNKNOWN;

    d.SampleDesc.Count = 1u << (key.dsv >> 4);

    auto job = std::make_shared<PipelineJob>();
    job->hash = hash;
    job->layout = layout;
    job->desc = d;

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
        return done != pipelines_.end() ? done->second.Get() : nullptr;
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
    return done != pipelines_.end() ? done->second.Get() : nullptr;
  }
}