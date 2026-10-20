// The Vulkan backend of gfx

#include "gpu/native/gfx/vulkan/vulkan_gfx.h"

#include <rex/logging.h>
#include <rex/ui/vulkan/mem_alloc.h>
#include <rex/ui/vulkan/provider.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sst::gfx::vulkan
{
  namespace
  {
    using rex::ui::vulkan::VulkanDevice;
    using rex::ui::vulkan::VulkanProvider;

    // Device functions, loaded here (the SDK's table only has what its own backends use)
#define SST_VK_FUNCTIONS(X)         \
  X(vkCreateImageView)              \
  X(vkDestroyImageView)             \
  X(vkCreateSampler)                \
  X(vkDestroySampler)               \
  X(vkCreateDescriptorSetLayout)    \
  X(vkDestroyDescriptorSetLayout)   \
  X(vkCreateDescriptorPool)         \
  X(vkDestroyDescriptorPool)        \
  X(vkAllocateDescriptorSets)       \
  X(vkUpdateDescriptorSets)         \
  X(vkCreatePipelineLayout)         \
  X(vkDestroyPipelineLayout)        \
  X(vkCreateShaderModule)           \
  X(vkDestroyShaderModule)          \
  X(vkCreateGraphicsPipelines)      \
  X(vkDestroyPipeline)              \
  X(vkCreatePipelineCache)          \
  X(vkDestroyPipelineCache)         \
  X(vkGetPipelineCacheData)         \
  X(vkCreateQueryPool)              \
  X(vkDestroyQueryPool)             \
  X(vkCreateCommandPool)            \
  X(vkDestroyCommandPool)           \
  X(vkResetCommandPool)             \
  X(vkAllocateCommandBuffers)       \
  X(vkBeginCommandBuffer)           \
  X(vkEndCommandBuffer)             \
  X(vkQueueSubmit)                  \
  X(vkCreateSemaphore)              \
  X(vkDestroySemaphore)             \
  X(vkGetSemaphoreCounterValue)     \
  X(vkWaitSemaphores)               \
  X(vkGetBufferDeviceAddress)       \
  X(vkDeviceWaitIdle)               \
  X(vkCmdPipelineBarrier)           \
  X(vkCmdBeginRendering)            \
  X(vkCmdEndRendering)              \
  X(vkCmdClearAttachments)          \
  X(vkCmdSetViewport)               \
  X(vkCmdSetScissor)                \
  X(vkCmdSetBlendConstants)         \
  X(vkCmdBindPipeline)              \
  X(vkCmdBindDescriptorSets)        \
  X(vkCmdPushConstants)             \
  X(vkCmdBindVertexBuffers2)        \
  X(vkCmdBindIndexBuffer)           \
  X(vkCmdSetPrimitiveTopology)      \
  X(vkCmdSetPrimitiveRestartEnable) \
  X(vkCmdDraw)                      \
  X(vkCmdDrawIndexed)               \
  X(vkCmdCopyBufferToImage)         \
  X(vkCmdCopyBuffer)                \
  X(vkCmdWriteTimestamp)            \
  X(vkCmdCopyQueryPoolResults)      \
  X(vkCmdResetQueryPool)

    struct Fn
    {
#define SST_VK_DECLARE(name) PFN_##name name = nullptr;
      SST_VK_FUNCTIONS(SST_VK_DECLARE)
#undef SST_VK_DECLARE
      PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT = nullptr;
    };

    constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr uint32_t kPushBytes = 40;
    constexpr uint32_t kPushBlitSource = 32;
    constexpr uint32_t kPushBlitSampler = 36;
    constexpr uint32_t kSets = 4;

    uint32_t Binding(uint32_t slot)
    {
      return slot < 15 ? slot : 15;
    }

    bool IsDepth(VkFormat f)
    {
      return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT;
    }

    bool HasStencil(VkFormat f)
    {
      return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
    }

    VkImageAspectFlags FullAspect(VkFormat f)
    {
      if (!IsDepth(f))
        return VK_IMAGE_ASPECT_COLOR_BIT;

      return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }

    // A D3D12 resource state as Vulkan sees it
    struct Sync
    {
      VkPipelineStageFlags stage = 0;
      VkAccessFlags access = 0;
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    Sync MapState(D3D12_RESOURCE_STATES s)
    {
      Sync r;

      if (s == D3D12_RESOURCE_STATE_COMMON)
        return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};

      uint32_t layouts = 0; // distinct layouts asked for

      auto want = [&](VkImageLayout l)
      {
        if (r.layout != l)
          ++layouts;

        r.layout = l;
      };

      const auto has = [&](D3D12_RESOURCE_STATES bit)
      { return (uint32_t(s) & uint32_t(bit)) != 0; };

      if (has(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER))
      {
        r.stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        r.access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
      }

      if (has(D3D12_RESOURCE_STATE_INDEX_BUFFER))
      {
        r.stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        r.access |= VK_ACCESS_INDEX_READ_BIT;
      }

      if (has(D3D12_RESOURCE_STATE_RENDER_TARGET))
      {
        r.stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        r.access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        want(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      }

      if (has(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
      {
        r.stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        r.access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        want(VK_IMAGE_LAYOUT_GENERAL);
      }

      if (has(D3D12_RESOURCE_STATE_DEPTH_WRITE))
      {
        r.stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        r.access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        want(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      }

      const bool shader_read = has(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) || has(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

      if (has(D3D12_RESOURCE_STATE_DEPTH_READ))
      {
        r.stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        r.access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        want(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
      }

      if (has(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
      {
        r.stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        r.access |= VK_ACCESS_SHADER_READ_BIT;
      }

      if (has(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
      {
        r.stage |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        r.access |= VK_ACCESS_SHADER_READ_BIT;
      }

      // Depth read + sampling: the depth read-only layout allows both
      if (shader_read && !has(D3D12_RESOURCE_STATE_DEPTH_READ))
        want(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      if (has(D3D12_RESOURCE_STATE_STREAM_OUT))
      {
        r.stage |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        r.access |= VK_ACCESS_MEMORY_WRITE_BIT;
      }

      if (has(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT))
      {
        r.stage |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        r.access |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
      }

      if (has(D3D12_RESOURCE_STATE_COPY_DEST) || has(D3D12_RESOURCE_STATE_RESOLVE_DEST))
      {
        r.stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        r.access |= VK_ACCESS_TRANSFER_WRITE_BIT;
        want(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      }

      if (has(D3D12_RESOURCE_STATE_COPY_SOURCE) || has(D3D12_RESOURCE_STATE_RESOLVE_SOURCE))
      {
        r.stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        r.access |= VK_ACCESS_TRANSFER_READ_BIT;
        want(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      }

      if (layouts > 1)
        r.layout = VK_IMAGE_LAYOUT_GENERAL;

      if (!r.stage)
        r.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

      return r;
    }

    VkComponentSwizzle Component(uint32_t c, VkComponentSwizzle identity)
    {
      switch (c)
      {
      case 0:
        return VK_COMPONENT_SWIZZLE_R;

      case 1:
        return VK_COMPONENT_SWIZZLE_G;

      case 2:
        return VK_COMPONENT_SWIZZLE_B;

      case 3:
        return VK_COMPONENT_SWIZZLE_A;

      case 4:
        return VK_COMPONENT_SWIZZLE_ZERO;

      case 5:
        return VK_COMPONENT_SWIZZLE_ONE;

      default:
        return identity;
      }
    }

    VkComponentMapping Components(UINT mapping)
    {
      return {Component(mapping & 7, VK_COMPONENT_SWIZZLE_R), Component((mapping >> 3) & 7, VK_COMPONENT_SWIZZLE_G), Component((mapping >> 6) & 7, VK_COMPONENT_SWIZZLE_B), Component((mapping >> 9) & 7, VK_COMPONENT_SWIZZLE_A)};
    }

    VkBlendFactor BlendFactor(D3D12_BLEND b)
    {
      switch (b)
      {
      case D3D12_BLEND_ZERO:
        return VK_BLEND_FACTOR_ZERO;

      case D3D12_BLEND_ONE:
        return VK_BLEND_FACTOR_ONE;

      case D3D12_BLEND_SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;

      case D3D12_BLEND_INV_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;

      case D3D12_BLEND_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;

      case D3D12_BLEND_INV_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

      case D3D12_BLEND_DEST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;

      case D3D12_BLEND_INV_DEST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;

      case D3D12_BLEND_DEST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;

      case D3D12_BLEND_INV_DEST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;

      case D3D12_BLEND_SRC_ALPHA_SAT:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;

      case D3D12_BLEND_BLEND_FACTOR:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;

      case D3D12_BLEND_INV_BLEND_FACTOR:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;

      case D3D12_BLEND_SRC1_COLOR:
        return VK_BLEND_FACTOR_SRC1_COLOR;

      case D3D12_BLEND_INV_SRC1_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;

      case D3D12_BLEND_SRC1_ALPHA:
        return VK_BLEND_FACTOR_SRC1_ALPHA;

      case D3D12_BLEND_INV_SRC1_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;

      case D3D12_BLEND(20): // D3D12_BLEND_ALPHA_FACTOR (newer headers)
        return VK_BLEND_FACTOR_CONSTANT_ALPHA;

      case D3D12_BLEND(21): // D3D12_BLEND_INV_ALPHA_FACTOR
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;

      default:
        return VK_BLEND_FACTOR_ONE;
      }
    }

    VkBlendOp BlendOp(D3D12_BLEND_OP op)
    {
      switch (op)
      {
      case D3D12_BLEND_OP_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;

      case D3D12_BLEND_OP_REV_SUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;

      case D3D12_BLEND_OP_MIN:
        return VK_BLEND_OP_MIN;

      case D3D12_BLEND_OP_MAX:
        return VK_BLEND_OP_MAX;

      default:
        return VK_BLEND_OP_ADD;
      }
    }

    // D3D12 numbers its comparison functions and stencil ops from 1 in Vulkan's order
    VkCompareOp CompareOp(D3D12_COMPARISON_FUNC f)
    {
      return f >= D3D12_COMPARISON_FUNC_NEVER && f <= D3D12_COMPARISON_FUNC_ALWAYS ? VkCompareOp(f - 1) : VK_COMPARE_OP_ALWAYS;
    }

    VkStencilOp StencilOp(D3D12_STENCIL_OP op)
    {
      return op >= D3D12_STENCIL_OP_KEEP && op <= D3D12_STENCIL_OP_DECR ? VkStencilOp(op - 1) : VK_STENCIL_OP_KEEP;
    }

    VkSamplerAddressMode AddressMode(D3D12_TEXTURE_ADDRESS_MODE m, bool mirror_clamp)
    {
      switch (m)
      {
      case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;

      case D3D12_TEXTURE_ADDRESS_MODE_CLAMP:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

      case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

      case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
        return mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;

      default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      }
    }

    VkPrimitiveTopology Topology(D3D_PRIMITIVE_TOPOLOGY t)
    {
      switch (t)
      {
      case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

      case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

      case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

      default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      }
    }

    bool IsStrip(D3D_PRIMITIVE_TOPOLOGY t)
    {
      return t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || t == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }

    // Texel size: bytes per texel, or per 4x4 block for block-compressed formats (block = 4)
    struct FormatInfo
    {
      VkFormat vk = VK_FORMAT_UNDEFINED;
      uint32_t bytes = 0;
      uint32_t block = 1;
    };

    class Dev;

    class VulkanTexture final : public Texture
    {
    public:
      explicit VulkanTexture(Dev &dev);
      ~VulkanTexture() override;
      void SetName(std::string_view name) override;

      VkImageView AddView(const VkImageViewCreateInfo &info);

      Dev &dev_;
      std::shared_ptr<std::atomic<bool>> alive_; // false once the device is gone (objects released after it are leaked)
      VkImage image = VK_NULL_HANDLE;
      VmaAllocation allocation = nullptr;
      VkFormat format = VK_FORMAT_UNDEFINED;
      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      VkExtent3D extent = {1, 1, 1};
      uint32_t mips = 1, layers = 1;
      VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
      bool volume = false, cube_compatible = false;

    private:
      std::mutex views_mutex_;
      std::vector<VkImageView> views_; // every view made on the image, destroyed with it
    };

    class VulkanBuffer final : public Buffer
    {
    public:
      explicit VulkanBuffer(Dev &dev);
      ~VulkanBuffer() override;
      void SetName(std::string_view name) override;
      const uint8_t *Map(size_t begin, size_t end) override;
      void Unmap() override {}

      Dev &dev_;
      std::shared_ptr<std::atomic<bool>> alive_; // false once the device is gone (objects released after it are leaked)
      VkBuffer buffer = VK_NULL_HANDLE;
      VmaAllocation allocation = nullptr;
      uint8_t *mapped = nullptr;
      HeapType heap = HeapType::kDefault;
    };

    class VulkanPipeline final : public Pipeline
    {
    public:
      explicit VulkanPipeline(Dev &dev);
      ~VulkanPipeline() override;

      Dev &dev_;
      std::shared_ptr<std::atomic<bool>> alive_; // false once the device is gone (objects released after it are leaked)
      VkPipeline pipeline = VK_NULL_HANDLE;
      bool strip_cut = false;
      bool vertex_input = false; // has vertex bindings (and so the dynamic stride state)
    };

    class VulkanTimestampPool final : public TimestampPool
    {
    public:
      explicit VulkanTimestampPool(Dev &dev);
      ~VulkanTimestampPool() override;

      Dev &dev_;
      std::shared_ptr<std::atomic<bool>> alive_; // false once the device is gone (objects released after it are leaked)
      VkQueryPool pool = VK_NULL_HANDLE;
      uint32_t count = 0;
    };

    // An RTV / DSV table entry
    struct TargetView
    {
      VkImageView view = VK_NULL_HANDLE;
      VkExtent2D extent = {0, 0};
      VkFormat format = VK_FORMAT_UNDEFINED;
      VkImageAspectFlags aspect = 0;
    };

    class VulkanCommandList;

    class Dev final : public Device
    {
    public:
      explicit Dev(const VulkanProvider &provider) : vk_(*provider.vulkan_device()) {}
      ~Dev() override;

      bool Initialize();

      Backend backend() const override
      {
        return Backend::kVulkan;
      }

      ShaderFormat shader_format() const override
      {
        return ShaderFormat::kSpirv;
      }

      TextureRef CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE *clear) override;
      BufferRef CreateBuffer(uint64_t size, HeapType heap, D3D12_RESOURCE_STATES initial) override;
      uint64_t TextureBytes(const D3D12_RESOURCE_DESC &desc) override;
      void Footprints(const D3D12_RESOURCE_DESC &desc, uint32_t first, uint32_t count, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints, uint32_t *rows, uint64_t *row_bytes, uint64_t *total) override;
      bool SupportsSamples(DXGI_FORMAT format, uint32_t samples) override;

      bool CreateTables(const TableSizes &sizes) override;
      void WriteSrv(uint32_t slot, Texture &texture, const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) override;
      void WriteSampler(uint32_t slot, const D3D12_SAMPLER_DESC &desc) override;
      void WriteRtv(uint32_t slot, Texture &texture, const D3D12_RENDER_TARGET_VIEW_DESC *desc) override;
      void WriteDsv(uint32_t slot, Texture &texture, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc) override;
      bool CreateLayouts() override;

      PipelineRef CreatePipeline(const PipelineDesc &desc, int32_t *error) override
      {
        return BuildPipeline(desc, VK_NULL_HANDLE, error);
      }

      std::unique_ptr<PipelineCache> OpenPipelineCache(const std::filesystem::path &path) override;

      TimestampPoolRef CreateTimestampPool(uint32_t count) override;

      uint64_t TimestampFrequency() override
      {
        return timestamp_period_ > 0 ? uint64_t(1e9 / double(timestamp_period_)) : 0;
      }

      std::unique_ptr<CommandList> CreateCommandList(uint32_t slots) override;
      uint64_t Submit(CommandList &list) override;
      uint64_t CompletedFence() override;
      void WaitFence(uint64_t value) override;

      uint64_t LastSubmitted() const override
      {
        return submitted_.load(std::memory_order_acquire);
      }

      // For the objects and the command list
      const Fn &fn() const
      {
        return fn_;
      }

      VkDevice device() const
      {
        return device_;
      }

      VmaAllocator allocator() const
      {
        return allocator_;
      }

      const VulkanDevice &vk() const
      {
        return vk_;
      }

      VkPipelineLayout layout() const
      {
        return layout_;
      }

      const VkDescriptorSet *sets() const
      {
        return sets_;
      }

      bool has_sets() const
      {
        return sets_[0] != VK_NULL_HANDLE;
      }

      uint32_t blit_sampler() const
      {
        return blit_sampler_slot_;
      }

      const TargetView &Rtv(uint32_t slot) const
      {
        return slot < rtvs_.size() ? rtvs_[slot] : kNoTarget;
      }

      const TargetView &Dsv(uint32_t slot) const
      {
        return slot < dsvs_.size() ? dsvs_[slot] : kNoTarget;
      }

      // Destroys an object once the GPU is past everything submitted so far (and the list being recorded)
      void Retire(std::function<void()> destroy);

      // Command buffer for work that must run before the next submitted list (initial layouts, query resets). Call under SetupLock
      VkCommandBuffer SetupCommands();

      std::unique_lock<std::mutex> SetupLock()
      {
        return std::unique_lock<std::mutex>(setup_mutex_);
      }

      // GPU address -> buffer (vertex / index buffers are bound by address)
      void TrackBuffer(VulkanBuffer *buffer);
      void ForgetBuffer(VulkanBuffer *buffer);
      VulkanBuffer *FindBuffer(GpuAddress address, VkDeviceSize *offset);

      FormatInfo Format(DXGI_FORMAT f) const;
      VkFormatFeatureFlags Features(VkFormat f);

      PipelineRef BuildPipeline(const PipelineDesc &desc, VkPipelineCache cache, int32_t *error);

      template <class T>
      void Name(VkObjectType type, T handle, std::string_view name) const
      {
        const std::string s(name);
        vk_.SetObjectName(type, handle, s.c_str());
      }

      const VkPhysicalDeviceProperties &physical_properties() const
      {
        return physical_properties_;
      }

    private:
      static inline const TargetView kNoTarget = {};

      bool Load();
      void Collect();
      VkShaderModule Module(const ShaderCode &code);

      const VulkanDevice &vk_;
      VkDevice device_ = VK_NULL_HANDLE;
      Fn fn_;
      bool loaded_ = false;

    public:
      const std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    private:
      VmaAllocator allocator_ = nullptr;
      uint32_t family_ = 0;
      VkPhysicalDeviceProperties physical_properties_ = {};
      float timestamp_period_ = 0;
      uint32_t timestamp_bits_ = 0;
      VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;

      VkSemaphore timeline_ = VK_NULL_HANDLE;
      std::atomic<uint64_t> submitted_{0};

      std::mutex garbage_mutex_;
      std::vector<std::pair<uint64_t, std::function<void()>>> garbage_;

      struct SetupBuffer
      {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        uint64_t fence = 0;
      };

      std::mutex setup_mutex_;
      SetupBuffer setup_;                   // being recorded (cb null: none)
      std::vector<SetupBuffer> setup_done_; // submitted, reused once their fence completes

      std::shared_mutex buffers_mutex_;
      std::map<GpuAddress, VulkanBuffer *> buffers_;

      std::mutex formats_mutex_;
      std::unordered_map<uint32_t, VkFormatFeatureFlags> formats_;

      // View tables
      std::mutex tables_mutex_;
      VkDescriptorSetLayout set_layouts_[kSets] = {};
      VkDescriptorPool pool_ = VK_NULL_HANDLE;
      VkDescriptorSet sets_[kSets] = {};
      uint32_t srv_count_ = 0, sampler_count_ = 0, blit_sampler_slot_ = 0;
      std::vector<VkSampler> samplers_;
      std::vector<TargetView> rtvs_, dsvs_;
      VkPipelineLayout layout_ = VK_NULL_HANDLE;
      std::unordered_set<uint64_t> warned_;

      bool WarnOnce(uint64_t key)
      {
        return warned_.insert(key).second;
      }
    };

    // objects
    VulkanTexture::VulkanTexture(Dev &dev) : dev_(dev), alive_(dev.alive) {}
    VulkanBuffer::VulkanBuffer(Dev &dev) : dev_(dev), alive_(dev.alive) {}
    VulkanPipeline::VulkanPipeline(Dev &dev) : dev_(dev), alive_(dev.alive) {}
    VulkanTimestampPool::VulkanTimestampPool(Dev &dev) : dev_(dev), alive_(dev.alive) {}

    VulkanTexture::~VulkanTexture()
    {
      if (!*alive_)
        return;

      std::vector<VkImageView> views;

      {
        std::lock_guard lock(views_mutex_);
        views.swap(views_);
      }

      Dev &dev = dev_;
      VkImage image_ = image;
      VmaAllocation allocation_ = allocation;

      dev.Retire([&dev, views = std::move(views), image_, allocation_]
                 {
        for (VkImageView v : views)
          dev.fn().vkDestroyImageView(dev.device(), v, nullptr);

        if (image_)
          vmaDestroyImage(dev.allocator(), image_, allocation_); });
    }

    void VulkanTexture::SetName(std::string_view name)
    {
      dev_.Name(VK_OBJECT_TYPE_IMAGE, image, name);
    }

    VkImageView VulkanTexture::AddView(const VkImageViewCreateInfo &info)
    {
      VkImageView view = VK_NULL_HANDLE;

      if (dev_.fn().vkCreateImageView(dev_.device(), &info, nullptr, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;

      std::lock_guard lock(views_mutex_);
      views_.push_back(view);
      return view;
    }

    VulkanBuffer::~VulkanBuffer()
    {
      if (!*alive_)
        return;

      dev_.ForgetBuffer(this);
      Dev &dev = dev_;
      VkBuffer buffer_ = buffer;
      VmaAllocation allocation_ = allocation;

      dev.Retire([&dev, buffer_, allocation_]
                 {
        if (buffer_)
          vmaDestroyBuffer(dev.allocator(), buffer_, allocation_); });
    }

    void VulkanBuffer::SetName(std::string_view name)
    {
      dev_.Name(VK_OBJECT_TYPE_BUFFER, buffer, name);
    }

    const uint8_t *VulkanBuffer::Map(size_t begin, size_t end)
    {
      if (!mapped)
        return nullptr;

      if (end > begin)
        vmaInvalidateAllocation(dev_.allocator(), allocation, begin, end - begin);

      return mapped;
    }

    VulkanPipeline::~VulkanPipeline()
    {
      if (!*alive_)
        return;

      Dev &dev = dev_;
      VkPipeline p = pipeline;

      dev.Retire([&dev, p]
                 {
        if (p)
          dev.fn().vkDestroyPipeline(dev.device(), p, nullptr); });
    }

    VulkanTimestampPool::~VulkanTimestampPool()
    {
      if (!*alive_)
        return;

      Dev &dev = dev_;
      VkQueryPool p = pool;

      dev.Retire([&dev, p]
                 {
        if (p)
          dev.fn().vkDestroyQueryPool(dev.device(), p, nullptr); });
    }

    // command list
    class VulkanCommandList final : public CommandList
    {
    public:
      explicit VulkanCommandList(Dev &dev);
      ~VulkanCommandList() override;
      bool Create(uint32_t slots);

      void Begin(uint32_t slot, std::string_view name) override;
      void End() override;

      void Barrier(Texture &texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override;
      void Barrier(Buffer &buffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override;

      void ClearRtv(uint32_t rtv, const float color[4], const D3D12_RECT *rects, uint32_t rect_count) override;
      void ClearDsv(uint32_t dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil, const D3D12_RECT *rects, uint32_t rect_count) override;
      void SetTargets(uint32_t rtv, uint32_t dsv) override;

      void SetViewport(const D3D12_VIEWPORT &v) override;
      void SetScissor(const D3D12_RECT &rect) override;

      void SetBlendFactor(const float factor[4]) override
      {
        fn_.vkCmdSetBlendConstants(cb_, factor);
      }

      void SetLayout(Layout layout) override;
      void SetPipeline(Pipeline &pipeline) override;
      void SetDrawConstants(GpuAddress vs, GpuAddress ps, GpuAddress shared) override;
      void SetBlitConstants(const int32_t *values, uint32_t count) override;
      void SetBlitSource(uint32_t srv) override;

      void SetVertexBuffer(uint32_t slot, GpuAddress address, uint32_t size, uint32_t stride) override;
      void SetIndexBuffer(GpuAddress address, uint32_t size, DXGI_FORMAT format) override;
      void SetTopology(D3D_PRIMITIVE_TOPOLOGY topology) override;
      void Draw(uint32_t vertices, uint32_t first_vertex) override;
      void DrawIndexed(uint32_t indices, uint32_t first_index, int32_t base_vertex) override;

      void CopyBufferToTexture(Texture &dst, uint32_t subresource, Buffer &src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) override;
      void CopyBuffer(Buffer &dst, uint64_t dst_offset, Buffer &src, uint64_t src_offset, uint64_t size) override;
      void CopyBufferAll(Buffer &dst, Buffer &src) override;

      void WriteTimestamp(TimestampPool &pool, uint32_t index) override;
      void ResolveTimestamps(TimestampPool &pool, uint32_t first, uint32_t count, Buffer &dst, uint64_t dst_offset) override;

      void Marker(std::string_view text) override;

      VkCommandBuffer cb() const
      {
        return cb_;
      }

    private:
      void BeginRendering(uint32_t rtv, uint32_t dsv);
      void EndRendering();
      bool PrepareDraw();

      Dev &dev_;
      std::shared_ptr<std::atomic<bool>> alive_; // false once the device is gone (objects released after it are leaked)
      const Fn &fn_;

      struct Slot
      {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
      };

      std::vector<Slot> slots_;
      VkCommandBuffer cb_ = VK_NULL_HANDLE;

      uint32_t want_rtv_ = kNoView, want_dsv_ = kNoView; // SetTargets
      bool rendering_ = false;
      uint32_t cur_rtv_ = kNoView, cur_dsv_ = kNoView; // the render pass instance's
      VkExtent2D area_ = {0, 0};

      bool blit_ = false;
      GpuAddress draw_constants_[3] = {};
      VulkanPipeline *pipeline_ = nullptr;
      D3D_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      int restart_ = -1; // primitive restart as last set (-1: unknown)
      bool viewport_empty_ = false;

      // Vertex buffers as bound. Binding a pipeline without vertex input (a blit) makes the dynamic strides undefined in Vulkan; they are bound again before the next draw that has vertex input
      struct VertexBinding
      {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0, stride = 0;
      };

      VertexBinding vertex_bindings_[16];
      bool strides_lost_ = false;
    };

    VulkanCommandList::VulkanCommandList(Dev &dev) : dev_(dev), alive_(dev.alive), fn_(dev.fn()) {}

    VulkanCommandList::~VulkanCommandList()
    {
      if (!*alive_)
        return;

      for (Slot &s : slots_)
      {
        Dev &dev = dev_;
        VkCommandPool pool = s.pool;
        dev.Retire([&dev, pool]
                   { dev.fn().vkDestroyCommandPool(dev.device(), pool, nullptr); });
      }
    }

    bool VulkanCommandList::Create(uint32_t slots)
    {
      slots_.resize(std::max(slots, 1u));

      for (Slot &s : slots_)
      {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = dev_.vk().queue_family_graphics_compute();

        if (fn_.vkCreateCommandPool(dev_.device(), &pi, nullptr, &s.pool) != VK_SUCCESS)
          return false;

        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;

        if (fn_.vkAllocateCommandBuffers(dev_.device(), &ai, &s.cb) != VK_SUCCESS)
          return false;
      }

      return true;
    }

    void VulkanCommandList::Begin(uint32_t slot, std::string_view name)
    {
      Slot &s = slots_[slot % slots_.size()];
      fn_.vkResetCommandPool(dev_.device(), s.pool, 0);
      cb_ = s.cb;
      VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      fn_.vkBeginCommandBuffer(cb_, &bi);

      if (!name.empty())
        dev_.Name(VK_OBJECT_TYPE_COMMAND_BUFFER, cb_, name);

      rendering_ = false;
      want_rtv_ = want_dsv_ = cur_rtv_ = cur_dsv_ = kNoView;
      blit_ = false;
      pipeline_ = nullptr;
      std::memset(draw_constants_, 0, sizeof(draw_constants_));
      viewport_empty_ = false;

      for (VertexBinding &b : vertex_bindings_)
        b = {};

      strides_lost_ = false;

      if (dev_.has_sets())
        fn_.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.layout(), 0, kSets, dev_.sets(), 0, nullptr);

      // Dynamic state every pipeline has: start from D3D12's defaults
      const float zero[4] = {0, 0, 0, 0};
      fn_.vkCmdSetBlendConstants(cb_, zero);
      topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      fn_.vkCmdSetPrimitiveTopology(cb_, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
      fn_.vkCmdSetPrimitiveRestartEnable(cb_, VK_FALSE);
      restart_ = 0;
      const VkViewport vp = {0, 0, 1, 1, 0, 1};
      fn_.vkCmdSetViewport(cb_, 0, 1, &vp);
      const VkRect2D sc = {{0, 0}, {1, 1}};
      fn_.vkCmdSetScissor(cb_, 0, 1, &sc);
    }

    void VulkanCommandList::End()
    {
      EndRendering();

      // Readback buffers (timestamps, captures) are read by the CPU once the fence passes
      VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      fn_.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      fn_.vkEndCommandBuffer(cb_);
    }

    void VulkanCommandList::Barrier(Texture &texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      auto &t = static_cast<VulkanTexture &>(texture);
      const Sync a = MapState(before), b = MapState(after);
      EndRendering();
      VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      ib.srcAccessMask = a.access;
      ib.dstAccessMask = b.access;
      ib.oldLayout = a.layout;
      ib.newLayout = b.layout;
      ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ib.image = t.image;
      ib.subresourceRange = {t.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
      fn_.vkCmdPipelineBarrier(cb_, a.stage, b.stage, 0, 0, nullptr, 0, nullptr, 1, &ib);
    }

    void VulkanCommandList::Barrier(Buffer &buffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      auto &bf = static_cast<VulkanBuffer &>(buffer);
      const Sync a = MapState(before), b = MapState(after);
      EndRendering();
      VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      bb.srcAccessMask = a.access;
      bb.dstAccessMask = b.access;
      bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bb.buffer = bf.buffer;
      bb.offset = 0;
      bb.size = VK_WHOLE_SIZE;
      fn_.vkCmdPipelineBarrier(cb_, a.stage, b.stage, 0, 0, nullptr, 1, &bb, 0, nullptr);
    }

    void VulkanCommandList::BeginRendering(uint32_t rtv, uint32_t dsv)
    {
      const TargetView &c = dev_.Rtv(rtv);
      const TargetView &d = dev_.Dsv(dsv);
      const bool has_c = rtv != kNoView && c.view, has_d = dsv != kNoView && d.view;

      if (!has_c && !has_d)
        return;

      VkExtent2D extent = {UINT32_MAX, UINT32_MAX};

      if (has_c)
        extent = {std::min(extent.width, c.extent.width), std::min(extent.height, c.extent.height)};

      if (has_d)
        extent = {std::min(extent.width, d.extent.width), std::min(extent.height, d.extent.height)};

      VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      color.imageView = c.view;
      color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depth.imageView = d.view;
      depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, extent};
      ri.layerCount = 1;
      ri.colorAttachmentCount = has_c ? 1 : 0;
      ri.pColorAttachments = has_c ? &color : nullptr;
      ri.pDepthAttachment = has_d && (d.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &depth : nullptr;
      ri.pStencilAttachment = has_d && (d.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &depth : nullptr;
      fn_.vkCmdBeginRendering(cb_, &ri);
      rendering_ = true;
      cur_rtv_ = has_c ? rtv : kNoView;
      cur_dsv_ = has_d ? dsv : kNoView;
      area_ = extent;
    }

    void VulkanCommandList::EndRendering()
    {
      if (!rendering_)
        return;

      fn_.vkCmdEndRendering(cb_);
      rendering_ = false;
      cur_rtv_ = cur_dsv_ = kNoView;
    }

    void VulkanCommandList::SetTargets(uint32_t rtv, uint32_t dsv)
    {
      want_rtv_ = rtv;
      want_dsv_ = dsv;
    }

    bool VulkanCommandList::PrepareDraw()
    {
      if (!pipeline_ || viewport_empty_)
        return false;

      const uint32_t rtv = want_rtv_ != kNoView && dev_.Rtv(want_rtv_).view ? want_rtv_ : kNoView;
      const uint32_t dsv = want_dsv_ != kNoView && dev_.Dsv(want_dsv_).view ? want_dsv_ : kNoView;

      if (rendering_ && (cur_rtv_ != rtv || cur_dsv_ != dsv))
        EndRendering();

      if (!rendering_)
        BeginRendering(rtv, dsv);

      if (!rendering_)
        return false;

      const int restart = pipeline_->strip_cut && IsStrip(topology_) ? 1 : 0;

      if (restart != restart_)
      {
        fn_.vkCmdSetPrimitiveRestartEnable(cb_, restart ? VK_TRUE : VK_FALSE);
        restart_ = restart;
      }

      return true;
    }

    namespace
    {
      // Clear rectangles in the render area (D3D12 clips them to the view)
      uint32_t ClearRects(const D3D12_RECT *rects, uint32_t count, VkExtent2D area, std::vector<VkClearRect> &out)
      {
        out.clear();

        if (!count)
        {
          out.push_back({{{0, 0}, area}, 0, 1});
          return 1;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
          const int32_t l = std::clamp<int32_t>(rects[i].left, 0, int32_t(area.width)), t = std::clamp<int32_t>(rects[i].top, 0, int32_t(area.height));
          const int32_t r = std::clamp<int32_t>(rects[i].right, 0, int32_t(area.width)), b = std::clamp<int32_t>(rects[i].bottom, 0, int32_t(area.height));

          if (r > l && b > t)
            out.push_back({{{l, t}, {uint32_t(r - l), uint32_t(b - t)}}, 0, 1});
        }

        return uint32_t(out.size());
      }
    }

    void VulkanCommandList::ClearRtv(uint32_t rtv, const float color[4], const D3D12_RECT *rects, uint32_t rect_count)
    {
      if (!dev_.Rtv(rtv).view)
        return;

      const bool inside = rendering_ && cur_rtv_ == rtv;

      if (!inside)
      {
        EndRendering();
        BeginRendering(rtv, kNoView);
      }

      std::vector<VkClearRect> r;

      if (ClearRects(rects, rect_count, area_, r))
      {
        VkClearAttachment ca = {};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        std::memcpy(ca.clearValue.color.float32, color, sizeof(float) * 4);
        fn_.vkCmdClearAttachments(cb_, 1, &ca, uint32_t(r.size()), r.data());
      }

      if (!inside)
        EndRendering();
    }

    void VulkanCommandList::ClearDsv(uint32_t dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil, const D3D12_RECT *rects, uint32_t rect_count)
    {
      const TargetView &d = dev_.Dsv(dsv);

      if (!d.view)
        return;

      VkImageAspectFlags aspect = 0;

      if (uint32_t(flags) & D3D12_CLEAR_FLAG_DEPTH)
        aspect |= d.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;

      if (uint32_t(flags) & D3D12_CLEAR_FLAG_STENCIL)
        aspect |= d.aspect & VK_IMAGE_ASPECT_STENCIL_BIT;

      if (!aspect)
        return;

      const bool inside = rendering_ && cur_dsv_ == dsv;

      if (!inside)
      {
        EndRendering();
        BeginRendering(kNoView, dsv);
      }

      std::vector<VkClearRect> r;

      if (ClearRects(rects, rect_count, area_, r))
      {
        VkClearAttachment ca = {};
        ca.aspectMask = aspect;
        ca.clearValue.depthStencil = {depth, stencil};
        fn_.vkCmdClearAttachments(cb_, 1, &ca, uint32_t(r.size()), r.data());
      }

      if (!inside)
        EndRendering();
    }

    void VulkanCommandList::SetViewport(const D3D12_VIEWPORT &v)
    {
      // Vulkan rejects empty viewports; D3D12 draws nothing through one
      viewport_empty_ = !(v.Width > 0) || !(v.Height > 0);

      if (viewport_empty_)
        return;

      const VkViewport vp = {v.TopLeftX, v.TopLeftY, v.Width, v.Height, std::clamp(v.MinDepth, 0.0f, 1.0f), std::clamp(v.MaxDepth, 0.0f, 1.0f)};
      fn_.vkCmdSetViewport(cb_, 0, 1, &vp);
    }

    void VulkanCommandList::SetScissor(const D3D12_RECT &rect)
    {
      const int32_t l = std::max<int32_t>(rect.left, 0), t = std::max<int32_t>(rect.top, 0);
      const VkRect2D sc = {{l, t}, {uint32_t(std::max<int32_t>(rect.right - l, 0)), uint32_t(std::max<int32_t>(rect.bottom - t, 0))}};
      fn_.vkCmdSetScissor(cb_, 0, 1, &sc);
    }

    void VulkanCommandList::SetLayout(Layout layout)
    {
      // One pipeline layout serves both: only the push constants differ
      const bool blit = layout == Layout::kBlit;

      if (blit)
      {
        const uint32_t sampler = dev_.blit_sampler();
        fn_.vkCmdPushConstants(cb_, dev_.layout(), kStages, kPushBlitSampler, 4, &sampler);
      }
      else if (blit_)
      {
        // Back from a blit, which overwrote the draw constants
        fn_.vkCmdPushConstants(cb_, dev_.layout(), kStages, 0, sizeof(draw_constants_), draw_constants_);
      }

      blit_ = blit;
    }

    void VulkanCommandList::SetPipeline(Pipeline &pipeline)
    {
      auto &p = static_cast<VulkanPipeline &>(pipeline);
      fn_.vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
      pipeline_ = &p;

      if (!p.vertex_input)
      {
        strides_lost_ = true;
      }
      else if (strides_lost_)
      {
        for (uint32_t i = 0; i < 16; ++i)
        {
          const VertexBinding &b = vertex_bindings_[i];

          if (b.buffer)
            fn_.vkCmdBindVertexBuffers2(cb_, i, 1, &b.buffer, &b.offset, &b.size, &b.stride);
        }

        strides_lost_ = false;
      }
    }

    void VulkanCommandList::SetDrawConstants(GpuAddress vs, GpuAddress ps, GpuAddress shared)
    {
      draw_constants_[0] = vs;
      draw_constants_[1] = ps;
      draw_constants_[2] = shared;
      fn_.vkCmdPushConstants(cb_, dev_.layout(), kStages, 0, sizeof(draw_constants_), draw_constants_);
    }

    void VulkanCommandList::SetBlitConstants(const int32_t *values, uint32_t count)
    {
      count = std::min(count, 8u);

      if (count)
        fn_.vkCmdPushConstants(cb_, dev_.layout(), kStages, 0, count * 4, values);
    }

    void VulkanCommandList::SetBlitSource(uint32_t srv)
    {
      fn_.vkCmdPushConstants(cb_, dev_.layout(), kStages, kPushBlitSource, 4, &srv);
    }

    void VulkanCommandList::SetVertexBuffer(uint32_t slot, GpuAddress address, uint32_t size, uint32_t stride)
    {
      VkDeviceSize offset = 0;
      VulkanBuffer *b = dev_.FindBuffer(address, &offset);

      if (!b)
        return;

      const VkDeviceSize bytes = std::min<VkDeviceSize>(size ? size : VK_WHOLE_SIZE, b->size - offset);
      const VkDeviceSize vk_stride = stride;
      const uint32_t binding = Binding(slot);
      fn_.vkCmdBindVertexBuffers2(cb_, binding, 1, &b->buffer, &offset, &bytes, &vk_stride);
      vertex_bindings_[binding] = {b->buffer, offset, bytes, vk_stride};
    }

    void VulkanCommandList::SetIndexBuffer(GpuAddress address, uint32_t /*size*/, DXGI_FORMAT format)
    {
      VkDeviceSize offset = 0;
      VulkanBuffer *b = dev_.FindBuffer(address, &offset);

      if (!b)
        return;

      fn_.vkCmdBindIndexBuffer(cb_, b->buffer, offset, format == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    }

    void VulkanCommandList::SetTopology(D3D_PRIMITIVE_TOPOLOGY topology)
    {
      if (topology == topology_)
        return;

      topology_ = topology;
      fn_.vkCmdSetPrimitiveTopology(cb_, Topology(topology));
    }

    void VulkanCommandList::Draw(uint32_t vertices, uint32_t first_vertex)
    {
      if (vertices && PrepareDraw())
        fn_.vkCmdDraw(cb_, vertices, 1, first_vertex, 0);
    }

    void VulkanCommandList::DrawIndexed(uint32_t indices, uint32_t first_index, int32_t base_vertex)
    {
      if (indices && PrepareDraw())
        fn_.vkCmdDrawIndexed(cb_, indices, 1, first_index, base_vertex, 0);
    }

    void VulkanCommandList::CopyBufferToTexture(Texture &dst, uint32_t subresource, Buffer &src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint)
    {
      auto &t = static_cast<VulkanTexture &>(dst);
      auto &b = static_cast<VulkanBuffer &>(src);
      EndRendering();
      const uint32_t mip = subresource % t.mips, layer = subresource / t.mips;
      const FormatInfo fi = dev_.Format(footprint.Footprint.Format);
      const uint32_t texel_bytes = std::max(fi.bytes, 1u);
      VkBufferImageCopy r = {};
      r.bufferOffset = footprint.Offset;
      r.bufferRowLength = footprint.Footprint.RowPitch / texel_bytes * fi.block;
      r.bufferImageHeight = footprint.Footprint.Height;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, t.volume ? 0 : layer, 1};

      // (Block-compressed footprints are rounded up to whole blocks; the copy must stop at the mip's edge)
      r.imageExtent = {std::min(footprint.Footprint.Width, std::max(t.extent.width >> mip, 1u)), std::min(footprint.Footprint.Height, std::max(t.extent.height >> mip, 1u)), t.volume ? std::min(footprint.Footprint.Depth, std::max(t.extent.depth >> mip, 1u)) : 1};
      fn_.vkCmdCopyBufferToImage(cb_, b.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }

    void VulkanCommandList::CopyBuffer(Buffer &dst, uint64_t dst_offset, Buffer &src, uint64_t src_offset, uint64_t size)
    {
      if (!size)
        return;

      EndRendering();
      const VkBufferCopy r = {src_offset, dst_offset, size};
      fn_.vkCmdCopyBuffer(cb_, static_cast<VulkanBuffer &>(src).buffer, static_cast<VulkanBuffer &>(dst).buffer, 1, &r);
    }

    void VulkanCommandList::CopyBufferAll(Buffer &dst, Buffer &src)
    {
      CopyBuffer(dst, 0, src, 0, std::min(dst.size, src.size));
    }

    void VulkanCommandList::WriteTimestamp(TimestampPool &pool, uint32_t index)
    {
      auto &p = static_cast<VulkanTimestampPool &>(pool);

      if (index < p.count)
        fn_.vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p.pool, index);
    }

    void VulkanCommandList::ResolveTimestamps(TimestampPool &pool, uint32_t first, uint32_t count, Buffer &dst, uint64_t dst_offset)
    {
      auto &p = static_cast<VulkanTimestampPool &>(pool);

      if (!count || first + count > p.count)
        return;

      EndRendering();
      fn_.vkCmdCopyQueryPoolResults(cb_, p.pool, first, count, static_cast<VulkanBuffer &>(dst).buffer, dst_offset, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

      // Reset for their next use (D3D12 query heaps need no reset); after the copy has read them
      fn_.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
      fn_.vkCmdResetQueryPool(cb_, p.pool, first, count);
    }

    void VulkanCommandList::Marker(std::string_view text)
    {
      if (!fn_.vkCmdInsertDebugUtilsLabelEXT)
        return;

      const std::string s(text);
      VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
      label.pLabelName = s.c_str();
      fn_.vkCmdInsertDebugUtilsLabelEXT(cb_, &label);
    }

    // pipeline cache (a VkPipelineCache in a file next to the D3D12 one)
    class VulkanPipelineCache final : public PipelineCache
    {
    public:
      VulkanPipelineCache(Dev &dev, std::filesystem::path path) : dev_(dev), path_(std::move(path)) {}

      ~VulkanPipelineCache() override
      {
        if (cache_)
          dev_.fn().vkDestroyPipelineCache(dev_.device(), cache_, nullptr);
      }

      bool Open()
      {
        std::vector<uint8_t> blob;

        {
          std::ifstream in(path_, std::ios::binary);

          if (in)
            blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // The header names the driver it came from (a cache from another GPU or driver is started over; drivers vary in how well they check this themselves)
        const VkPhysicalDeviceProperties &pp = dev_.physical_properties();

        if (blob.size() >= 16 + VK_UUID_SIZE)
        {
          uint32_t header[4];
          std::memcpy(header, blob.data(), sizeof(header));

          if (header[0] < 16 + VK_UUID_SIZE || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2] != pp.vendorID || header[3] != pp.deviceID || std::memcmp(blob.data() + 16, pp.pipelineCacheUUID, VK_UUID_SIZE) != 0)
          {
            REXLOG_INFO("[sst-native] pipeline cache {} is from another GPU or driver; starting a new one", path_.string());
            blob.clear();
          }
        }
        else
        {
          blob.clear();
        }

        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = blob.size();
        ci.pInitialData = blob.empty() ? nullptr : blob.data();

        if (dev_.fn().vkCreatePipelineCache(dev_.device(), &ci, nullptr, &cache_) != VK_SUCCESS)
        {
          ci.initialDataSize = 0;
          ci.pInitialData = nullptr;
          blob.clear();

          if (dev_.fn().vkCreatePipelineCache(dev_.device(), &ci, nullptr, &cache_) != VK_SUCCESS)
          {
            REXLOG_WARN("[sst-native] pipeline cache unavailable");
            return false;
          }
        }

        file_bytes_ = blob.size();
        REXLOG_INFO("[sst-native] pipeline cache: {} ({} KB)", path_.string(), blob.size() >> 10);
        return true;
      }

      // A VkPipelineCache has no names: every pipeline is created, the cache makes it fast
      PipelineRef Load(const std::wstring &, const PipelineDesc &) override
      {
        return nullptr;
      }

      PipelineRef Create(const std::wstring &, const PipelineDesc &desc, int32_t *error) override
      {
        PipelineRef p = dev_.BuildPipeline(desc, cache_, error);

        if (p)
          dirty_.store(true, std::memory_order_release);

        return p;
      }

      size_t Save() override
      {
        if (!dirty_.exchange(false))
          return 0;

        size_t size = 0;

        if (dev_.fn().vkGetPipelineCacheData(dev_.device(), cache_, &size, nullptr) != VK_SUCCESS || !size)
          return 0;

        std::vector<uint8_t> data(size);

        if (dev_.fn().vkGetPipelineCacheData(dev_.device(), cache_, &size, data.data()) != VK_SUCCESS)
          return 0;

        data.resize(size);
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        const auto tmp = std::filesystem::path(path_).concat(".tmp");

        {
          std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

          if (!out)
            return 0;

          out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));

          if (!out)
            return 0;
        }

        std::filesystem::rename(tmp, path_, ec);
        return ec ? 0 : data.size();
      }

      bool Dirty() const override
      {
        return dirty_.load(std::memory_order_acquire);
      }

      size_t FileBytes() const override
      {
        return file_bytes_;
      }

    private:
      Dev &dev_;
      std::filesystem::path path_;
      VkPipelineCache cache_ = VK_NULL_HANDLE;
      std::atomic<bool> dirty_{false};
      size_t file_bytes_ = 0;
    };

    // device
    Dev::~Dev()
    {
      alive->store(false);

      if (!loaded_)
        return; // (Initialize failed before anything was created)

      fn_.vkDeviceWaitIdle(device_);

      // Everything retired can go now
      std::vector<std::pair<uint64_t, std::function<void()>>> garbage;

      {
        std::lock_guard lock(garbage_mutex_);
        garbage.swap(garbage_);
      }

      for (auto &g : garbage)
        g.second();

      for (VkSampler s : samplers_)
      {
        if (s)
          fn_.vkDestroySampler(device_, s, nullptr);
      }

      if (layout_)
        fn_.vkDestroyPipelineLayout(device_, layout_, nullptr);

      if (pool_)
        fn_.vkDestroyDescriptorPool(device_, pool_, nullptr);

      for (VkDescriptorSetLayout l : set_layouts_)
      {
        if (l)
          fn_.vkDestroyDescriptorSetLayout(device_, l, nullptr);
      }

      if (setup_.pool)
        fn_.vkDestroyCommandPool(device_, setup_.pool, nullptr);

      for (SetupBuffer &s : setup_done_)
        fn_.vkDestroyCommandPool(device_, s.pool, nullptr);

      if (timeline_)
        fn_.vkDestroySemaphore(device_, timeline_, nullptr);

      if (allocator_)
        vmaDestroyAllocator(allocator_);
    }

    bool Dev::Load()
    {
      const auto &ifn = vk_.vulkan_instance()->functions();
      PFN_vkGetDeviceProcAddr gdpa = ifn.vkGetDeviceProcAddr;
      bool ok = true;

      // Core names (Vulkan 1.2 / 1.3), else the extension's
      static const std::pair<const char *, const char *> kAlternates[] = {
          {"vkCmdBeginRendering", "vkCmdBeginRenderingKHR"},
          {"vkCmdEndRendering", "vkCmdEndRenderingKHR"},
          {"vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR"},
          {"vkWaitSemaphores", "vkWaitSemaphoresKHR"},
          {"vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR"},
      };

      auto get = [&](const char *name) -> PFN_vkVoidFunction
      {
        PFN_vkVoidFunction f = gdpa(device_, name);

        if (!f)
        {
          for (const auto &[core, alt] : kAlternates)
          {
            if (std::strcmp(core, name) == 0)
              f = gdpa(device_, alt);
          }
        }

        if (!f)
        {
          REXLOG_ERROR("[sst-native] Vulkan: {} is not available", name);
          ok = false;
        }

        return f;
      };

#define SST_VK_LOAD(name) fn_.name = reinterpret_cast<PFN_##name>(get(#name));
      SST_VK_FUNCTIONS(SST_VK_LOAD)
#undef SST_VK_LOAD

      if (vk_.vulkan_instance()->extensions().ext_EXT_debug_utils)
        fn_.vkCmdInsertDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(ifn.vkGetInstanceProcAddr(vk_.vulkan_instance()->instance(), "vkCmdInsertDebugUtilsLabelEXT"));

      return ok;
    }

    bool Dev::Initialize()
    {
      device_ = vk_.device();
      family_ = vk_.queue_family_graphics_compute();
      const auto &p = vk_.properties();
      const auto &ifn = vk_.vulkan_instance()->functions();
      ifn.vkGetPhysicalDeviceProperties(vk_.physical_device(), &physical_properties_);

      // What the translated shaders and this backend rely on
      struct Need
      {
        bool have;
        const char *what;
      };

      const Need needs[] = {
          {VK_API_VERSION_MINOR(p.apiVersion) >= 3 || VK_API_VERSION_MAJOR(p.apiVersion) > 1, "Vulkan 1.3"},
          {p.dynamicRendering, "dynamic rendering"},
          {p.shaderInt64, "64-bit integers in shaders"},
          {p.bufferDeviceAddress, "buffer device addresses"},
          {p.runtimeDescriptorArray && p.descriptorBindingPartiallyBound && p.descriptorBindingVariableDescriptorCount && p.descriptorBindingSampledImageUpdateAfterBind, "bindless descriptor arrays"},
          {p.timelineSemaphore, "timeline semaphores"},
      };

      bool ok = true;

      for (const Need &n : needs)
      {
        if (!n.have)
        {
          REXLOG_ERROR("[sst-native] Vulkan: {} lacks {} (needed by the native renderer). Is the SDK patched (rexglue-sst.patch)?", p.deviceName, n.what);
          ok = false;
        }
      }

      if (!ok || !Load())
        return false;

      loaded_ = true;

      uint32_t families = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vk_.physical_device(), &families, nullptr);
      std::vector<VkQueueFamilyProperties> fp(families);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vk_.physical_device(), &families, fp.data());

      if (family_ < families)
        timestamp_bits_ = fp[family_].timestampValidBits;

      timestamp_period_ = physical_properties_.limits.timestampPeriod;

      allocator_ = rex::ui::vulkan::CreateVmaAllocator(&vk_, false);

      if (!allocator_)
        return false;

      VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      ti.initialValue = 0;
      VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      si.pNext = &ti;

      if (fn_.vkCreateSemaphore(device_, &si, nullptr, &timeline_) != VK_SUCCESS)
        return false;

      // D24S8 isn't a depth target everywhere (AMD): D32S8 stands in
      if (!(Features(VK_FORMAT_D24_UNORM_S8_UINT) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;

      REXLOG_INFO("[sst-native] Vulkan device: {} (Vulkan {}.{}.{}, driver {:08X}){}", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, d24s8_ != VK_FORMAT_D24_UNORM_S8_UINT ? ", D24S8 targets as D32S8" : "");
      return true;
    }

    VkFormatFeatureFlags Dev::Features(VkFormat f)
    {
      std::lock_guard lock(formats_mutex_);
      auto [it, fresh] = formats_.try_emplace(uint32_t(f), 0);

      if (fresh)
      {
        VkFormatProperties props = {};
        vk_.vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(vk_.physical_device(), f, &props);
        it->second = props.optimalTilingFeatures;
      }

      return it->second;
    }

    FormatInfo Dev::Format(DXGI_FORMAT f) const
    {
      switch (f)
      {
      case DXGI_FORMAT_R32G32B32A32_TYPELESS:

      case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};

      case DXGI_FORMAT_R32G32B32A32_UINT:
        return {VK_FORMAT_R32G32B32A32_UINT, 16};

      case DXGI_FORMAT_R32G32B32A32_SINT:
        return {VK_FORMAT_R32G32B32A32_SINT, 16};

      case DXGI_FORMAT_R32G32B32_TYPELESS:

      case DXGI_FORMAT_R32G32B32_FLOAT:
        return {VK_FORMAT_R32G32B32_SFLOAT, 12};

      case DXGI_FORMAT_R32G32B32_UINT:
        return {VK_FORMAT_R32G32B32_UINT, 12};

      case DXGI_FORMAT_R32G32B32_SINT:
        return {VK_FORMAT_R32G32B32_SINT, 12};

      case DXGI_FORMAT_R16G16B16A16_TYPELESS:

      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};

      case DXGI_FORMAT_R16G16B16A16_UNORM:
        return {VK_FORMAT_R16G16B16A16_UNORM, 8};

      case DXGI_FORMAT_R16G16B16A16_UINT:
        return {VK_FORMAT_R16G16B16A16_UINT, 8};

      case DXGI_FORMAT_R16G16B16A16_SNORM:
        return {VK_FORMAT_R16G16B16A16_SNORM, 8};

      case DXGI_FORMAT_R16G16B16A16_SINT:
        return {VK_FORMAT_R16G16B16A16_SINT, 8};

      case DXGI_FORMAT_R32G32_TYPELESS:

      case DXGI_FORMAT_R32G32_FLOAT:
        return {VK_FORMAT_R32G32_SFLOAT, 8};

      case DXGI_FORMAT_R32G32_UINT:
        return {VK_FORMAT_R32G32_UINT, 8};

      case DXGI_FORMAT_R32G32_SINT:
        return {VK_FORMAT_R32G32_SINT, 8};

      case DXGI_FORMAT_R32G8X24_TYPELESS:

      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:

      case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:

      case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return {VK_FORMAT_D32_SFLOAT_S8_UINT, 8};

      case DXGI_FORMAT_R10G10B10A2_TYPELESS:

      case DXGI_FORMAT_R10G10B10A2_UNORM:
        return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4};

      case DXGI_FORMAT_R10G10B10A2_UINT:
        return {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4};

      case DXGI_FORMAT_R11G11B10_FLOAT:
        return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4};

      case DXGI_FORMAT_R8G8B8A8_TYPELESS:

      case DXGI_FORMAT_R8G8B8A8_UNORM:
        return {VK_FORMAT_R8G8B8A8_UNORM, 4};

      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return {VK_FORMAT_R8G8B8A8_SRGB, 4};

      case DXGI_FORMAT_R8G8B8A8_UINT:
        return {VK_FORMAT_R8G8B8A8_UINT, 4};

      case DXGI_FORMAT_R8G8B8A8_SNORM:
        return {VK_FORMAT_R8G8B8A8_SNORM, 4};

      case DXGI_FORMAT_R8G8B8A8_SINT:
        return {VK_FORMAT_R8G8B8A8_SINT, 4};

      case DXGI_FORMAT_R16G16_TYPELESS:

      case DXGI_FORMAT_R16G16_FLOAT:
        return {VK_FORMAT_R16G16_SFLOAT, 4};

      case DXGI_FORMAT_R16G16_UNORM:
        return {VK_FORMAT_R16G16_UNORM, 4};

      case DXGI_FORMAT_R16G16_UINT:
        return {VK_FORMAT_R16G16_UINT, 4};

      case DXGI_FORMAT_R16G16_SNORM:
        return {VK_FORMAT_R16G16_SNORM, 4};

      case DXGI_FORMAT_R16G16_SINT:
        return {VK_FORMAT_R16G16_SINT, 4};

      case DXGI_FORMAT_R32_TYPELESS:

      case DXGI_FORMAT_R32_FLOAT:
        return {VK_FORMAT_R32_SFLOAT, 4};

      case DXGI_FORMAT_D32_FLOAT:
        return {VK_FORMAT_D32_SFLOAT, 4};

      case DXGI_FORMAT_R32_UINT:
        return {VK_FORMAT_R32_UINT, 4};

      case DXGI_FORMAT_R32_SINT:
        return {VK_FORMAT_R32_SINT, 4};

      case DXGI_FORMAT_R24G8_TYPELESS:

      case DXGI_FORMAT_D24_UNORM_S8_UINT:

      case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:

      case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return {d24s8_, 4};

      case DXGI_FORMAT_R8G8_TYPELESS:

      case DXGI_FORMAT_R8G8_UNORM:
        return {VK_FORMAT_R8G8_UNORM, 2};

      case DXGI_FORMAT_R8G8_UINT:
        return {VK_FORMAT_R8G8_UINT, 2};

      case DXGI_FORMAT_R8G8_SNORM:
        return {VK_FORMAT_R8G8_SNORM, 2};

      case DXGI_FORMAT_R8G8_SINT:
        return {VK_FORMAT_R8G8_SINT, 2};

      case DXGI_FORMAT_R16_TYPELESS:

      case DXGI_FORMAT_R16_FLOAT:
        return {VK_FORMAT_R16_SFLOAT, 2};

      case DXGI_FORMAT_D16_UNORM:
        return {VK_FORMAT_D16_UNORM, 2};

      case DXGI_FORMAT_R16_UNORM:
        return {VK_FORMAT_R16_UNORM, 2};

      case DXGI_FORMAT_R16_UINT:
        return {VK_FORMAT_R16_UINT, 2};

      case DXGI_FORMAT_R16_SNORM:
        return {VK_FORMAT_R16_SNORM, 2};

      case DXGI_FORMAT_R16_SINT:
        return {VK_FORMAT_R16_SINT, 2};

      case DXGI_FORMAT_R8_TYPELESS:

      case DXGI_FORMAT_R8_UNORM:

      case DXGI_FORMAT_A8_UNORM:
        return {VK_FORMAT_R8_UNORM, 1};

      case DXGI_FORMAT_R8_UINT:
        return {VK_FORMAT_R8_UINT, 1};

      case DXGI_FORMAT_R8_SNORM:
        return {VK_FORMAT_R8_SNORM, 1};

      case DXGI_FORMAT_R8_SINT:
        return {VK_FORMAT_R8_SINT, 1};

      case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4};

      case DXGI_FORMAT_BC1_TYPELESS:

      case DXGI_FORMAT_BC1_UNORM:
        return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4};

      case DXGI_FORMAT_BC1_UNORM_SRGB:
        return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4};

      case DXGI_FORMAT_BC2_TYPELESS:

      case DXGI_FORMAT_BC2_UNORM:
        return {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4};

      case DXGI_FORMAT_BC2_UNORM_SRGB:
        return {VK_FORMAT_BC2_SRGB_BLOCK, 16, 4};

      case DXGI_FORMAT_BC3_TYPELESS:

      case DXGI_FORMAT_BC3_UNORM:
        return {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4};

      case DXGI_FORMAT_BC3_UNORM_SRGB:
        return {VK_FORMAT_BC3_SRGB_BLOCK, 16, 4};

      case DXGI_FORMAT_BC4_TYPELESS:

      case DXGI_FORMAT_BC4_UNORM:
        return {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4};

      case DXGI_FORMAT_BC4_SNORM:
        return {VK_FORMAT_BC4_SNORM_BLOCK, 8, 4};

      case DXGI_FORMAT_BC5_TYPELESS:

      case DXGI_FORMAT_BC5_UNORM:
        return {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4};

      case DXGI_FORMAT_BC5_SNORM:
        return {VK_FORMAT_BC5_SNORM_BLOCK, 16, 4};

      case DXGI_FORMAT_BC6H_TYPELESS:

      case DXGI_FORMAT_BC6H_UF16:
        return {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4};

      case DXGI_FORMAT_BC6H_SF16:
        return {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4};

      case DXGI_FORMAT_BC7_TYPELESS:

      case DXGI_FORMAT_BC7_UNORM:
        return {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4};

      case DXGI_FORMAT_BC7_UNORM_SRGB:
        return {VK_FORMAT_BC7_SRGB_BLOCK, 16, 4};

      case DXGI_FORMAT_B5G6R5_UNORM:
        return {VK_FORMAT_R5G6B5_UNORM_PACK16, 2};

      case DXGI_FORMAT_B5G5R5A1_UNORM:
        return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2};

      case DXGI_FORMAT_B8G8R8A8_TYPELESS:

      case DXGI_FORMAT_B8G8R8A8_UNORM:

      case DXGI_FORMAT_B8G8R8X8_TYPELESS:

      case DXGI_FORMAT_B8G8R8X8_UNORM:
        return {VK_FORMAT_B8G8R8A8_UNORM, 4};

      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:

      case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return {VK_FORMAT_B8G8R8A8_SRGB, 4};

      case DXGI_FORMAT_B4G4R4A4_UNORM:
        return {VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2};

      default:
        return {};
      }
    }

    void Dev::Retire(std::function<void()> destroy)
    {
      std::lock_guard lock(garbage_mutex_);
      garbage_.emplace_back(submitted_.load(std::memory_order_acquire) + 1, std::move(destroy));
    }

    void Dev::Collect()
    {
      const uint64_t done = CompletedFence();
      std::vector<std::function<void()>> ready;

      {
        std::lock_guard lock(garbage_mutex_);
        auto keep = garbage_.begin();

        for (auto it = garbage_.begin(); it != garbage_.end(); ++it)
        {
          if (it->first <= done)
            ready.push_back(std::move(it->second));
          else
            *keep++ = std::move(*it);
        }

        garbage_.erase(keep, garbage_.end());
      }

      for (auto &f : ready)
        f();
    }

    VkCommandBuffer Dev::SetupCommands()
    {
      if (setup_.cb)
        return setup_.cb;

      // A finished setup buffer the GPU is done with, or a new one
      const uint64_t done = CompletedFence();

      for (size_t i = 0; i < setup_done_.size(); ++i)
      {
        if (setup_done_[i].fence <= done)
        {
          setup_ = setup_done_[i];
          setup_done_.erase(setup_done_.begin() + ptrdiff_t(i));
          fn_.vkResetCommandPool(device_, setup_.pool, 0);
          break;
        }
      }

      if (!setup_.pool)
      {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family_;

        if (fn_.vkCreateCommandPool(device_, &pi, nullptr, &setup_.pool) != VK_SUCCESS)
          return VK_NULL_HANDLE;

        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = setup_.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;

        if (fn_.vkAllocateCommandBuffers(device_, &ai, &setup_.cb) != VK_SUCCESS)
        {
          fn_.vkDestroyCommandPool(device_, setup_.pool, nullptr);
          setup_ = {};
          return VK_NULL_HANDLE;
        }
      }

      VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      fn_.vkBeginCommandBuffer(setup_.cb, &bi);
      return setup_.cb;
    }

    void Dev::TrackBuffer(VulkanBuffer *buffer)
    {
      if (!buffer->gpu)
        return;

      std::unique_lock lock(buffers_mutex_);
      buffers_[buffer->gpu] = buffer;
    }

    void Dev::ForgetBuffer(VulkanBuffer *buffer)
    {
      if (!buffer->gpu)
        return;

      std::unique_lock lock(buffers_mutex_);
      auto it = buffers_.find(buffer->gpu);

      if (it != buffers_.end() && it->second == buffer)
        buffers_.erase(it);
    }

    VulkanBuffer *Dev::FindBuffer(GpuAddress address, VkDeviceSize *offset)
    {
      if (!address)
        return nullptr;

      std::shared_lock lock(buffers_mutex_);
      auto it = buffers_.upper_bound(address);

      if (it == buffers_.begin())
        return nullptr;

      --it;

      if (address >= it->first + it->second->size)
        return nullptr;

      *offset = address - it->first;
      return it->second;
    }

    TextureRef Dev::CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE * /*clear*/)
    {
      const bool rt = (uint32_t(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
      const bool ds = (uint32_t(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
      const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      VkFormat format = Format(desc.Format).vk;

      // Typeless R32 / R24G8 with a depth-stencil flag are depth formats
      if (ds && desc.Format == DXGI_FORMAT_R32_TYPELESS)
        format = VK_FORMAT_D32_SFLOAT;

      if (ds && desc.Format == DXGI_FORMAT_R16_TYPELESS)
        format = VK_FORMAT_D16_UNORM;

      if (format == VK_FORMAT_UNDEFINED || (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && !volume))
      {
        REXLOG_WARN("[sst-native] Vulkan: no image for DXGI format {} / dimension {}", uint32_t(desc.Format), uint32_t(desc.Dimension));
        return nullptr;
      }

      const VkFormatFeatureFlags features = Features(format);
      const uint32_t samples = std::max(desc.SampleDesc.Count, 1u);

      if ((rt && !(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) || (ds && !(features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) || !(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      {
        REXLOG_WARN("[sst-native] Vulkan: format {} (DXGI {}) can't be {} on this GPU", uint32_t(format), uint32_t(desc.Format), rt ? "a render target" : ds ? "a depth target"
                                                                                                                                                             : "sampled");

        return nullptr;
      }

      const uint32_t width = uint32_t(std::max<UINT64>(desc.Width, 1)), height = std::max(desc.Height, 1u);
      const uint32_t depth_or_layers = std::max<uint32_t>(desc.DepthOrArraySize, 1);
      uint32_t mips = desc.MipLevels;

      if (!mips) // D3D12: the full chain
      {
        uint32_t m = std::max({width, height, volume ? depth_or_layers : 1u});
        mips = 1;

        while (m > 1)
          m >>= 1, ++mips;
      }

      VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      ci.format = format;
      ci.extent = {width, height, volume ? depth_or_layers : 1};
      ci.mipLevels = mips;
      ci.arrayLayers = volume ? 1 : depth_or_layers;
      ci.samples = VkSampleCountFlagBits(samples);
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

      if (samples == 1)
        ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      if (rt)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

      if (ds)
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

      const bool cube = !volume && samples == 1 && ci.arrayLayers >= 6 && ci.arrayLayers % 6 == 0 && width == height;

      if (cube)
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

      if (volume && rt) // slices are drawn to through 2D views
        ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;

      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      VmaAllocationCreateInfo ai = {};
      ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

      if (rt || ds)
        ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

      auto t = std::make_shared<VulkanTexture>(*this);

      if (vmaCreateImage(allocator_, &ci, &ai, &t->image, &t->allocation, nullptr) != VK_SUCCESS)
      {
        t->image = VK_NULL_HANDLE;
        return nullptr;
      }

      t->desc = desc;
      t->format = format;
      t->aspect = FullAspect(format);
      t->extent = ci.extent;
      t->mips = mips;
      t->layers = ci.arrayLayers;
      t->samples = ci.samples;
      t->volume = volume;
      t->cube_compatible = cube;

      // UNDEFINED -> the initial state, ahead of the next submitted list
      const Sync to = MapState(initial);

      {
        auto lock = SetupLock();

        if (VkCommandBuffer cb = SetupCommands())
        {
          VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          ib.srcAccessMask = 0;
          ib.dstAccessMask = to.access;
          ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          ib.newLayout = to.layout;
          ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          ib.image = t->image;
          ib.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
          fn_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, to.stage, 0, 0, nullptr, 0, nullptr, 1, &ib);
        }
      }

      return t;
    }

    BufferRef Dev::CreateBuffer(uint64_t size, HeapType heap, D3D12_RESOURCE_STATES /*initial*/)
    {
      VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bi.size = std::max<uint64_t>(size, 4);
      bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      VmaAllocationCreateInfo ai = {};

      switch (heap)
      {
      case HeapType::kUpload:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; // written by the CPU, read at submit: no flushes
        break;

      case HeapType::kReadback:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;

      default:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
      }

      auto b = std::make_shared<VulkanBuffer>(*this);
      VmaAllocationInfo info = {};

      if (vmaCreateBuffer(allocator_, &bi, &ai, &b->buffer, &b->allocation, &info) != VK_SUCCESS)
      {
        b->buffer = VK_NULL_HANDLE;
        return nullptr;
      }

      b->size = size;
      b->heap = heap;
      b->mapped = static_cast<uint8_t *>(info.pMappedData);

      if (heap == HeapType::kUpload)
        b->cpu = b->mapped;

      VkBufferDeviceAddressInfo da = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      da.buffer = b->buffer;
      b->gpu = fn_.vkGetBufferDeviceAddress(device_, &da);
      TrackBuffer(b.get());
      return b;
    }

    void Dev::Footprints(const D3D12_RESOURCE_DESC &desc, uint32_t first, uint32_t count, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints, uint32_t *rows, uint64_t *row_bytes, uint64_t *total)
    {
      // The same layout as D3D12's GetCopyableFootprints: rows padded to 256 bytes, subresources placed at 512-byte boundaries
      const FormatInfo fi = Format(desc.Format);
      const uint32_t bytes = std::max(fi.bytes, 1u), block = fi.block;
      const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      const uint32_t mips = std::max<uint32_t>(desc.MipLevels, 1);
      uint64_t offset = 0, end = 0;

      for (uint32_t i = 0; i < count; ++i)
      {
        const uint32_t mip = (first + i) % mips;
        const uint32_t w = std::max<uint32_t>(uint32_t(desc.Width) >> mip, 1), h = std::max<uint32_t>(desc.Height >> mip, 1);
        const uint32_t d = volume ? std::max<uint32_t>(uint32_t(desc.DepthOrArraySize) >> mip, 1) : 1;
        const uint32_t bw = (w + block - 1) / block, bh = (h + block - 1) / block;
        const uint32_t row = bw * bytes;
        const uint32_t pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);

        if (footprints)
        {
          footprints[i].Offset = offset;
          footprints[i].Footprint = {desc.Format, bw * block, bh * block, d, pitch};
        }

        if (rows)
          rows[i] = bh;

        if (row_bytes)
          row_bytes[i] = row;

        end = offset + uint64_t(pitch) * (uint64_t(bh) * d - 1) + row;
        offset += uint64_t(pitch) * bh * d;
      }

      if (total)
        *total = end;
    }

    uint64_t Dev::TextureBytes(const D3D12_RESOURCE_DESC &desc)
    {
      const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      const uint32_t subresources = std::max<uint32_t>(desc.MipLevels, 1) * (volume ? 1 : std::max<uint32_t>(desc.DepthOrArraySize, 1));
      uint64_t total = 0;
      Footprints(desc, 0, subresources, nullptr, nullptr, nullptr, &total);
      return total * std::max(desc.SampleDesc.Count, 1u);
    }

    bool Dev::SupportsSamples(DXGI_FORMAT format, uint32_t samples)
    {
      const VkFormat f = Format(format).vk;

      if (f == VK_FORMAT_UNDEFINED)
        return false;

      const auto &p = vk_.properties();
      const bool depth = IsDepth(f);
      const VkSampleCountFlags counts = depth ? (p.framebufferDepthSampleCounts & (HasStencil(f) ? p.framebufferStencilSampleCounts : ~0u)) : p.framebufferColorSampleCounts;
      const VkFormatFeatureFlags need = depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      return (Features(f) & need) && (counts & samples) && (p.sampledImageColorSampleCounts & samples);
    }

    bool Dev::CreateTables(const TableSizes &sizes)
    {
      srv_count_ = std::max(sizes.srv, 1u);
      sampler_count_ = std::max(sizes.sampler, 1u) + 1; // + the blit sampler
      blit_sampler_slot_ = sampler_count_ - 1;
      samplers_.assign(sampler_count_, VK_NULL_HANDLE);
      rtvs_.assign(sizes.rtv, TargetView{});
      dsvs_.assign(sizes.dsv, TargetView{});

      VkPhysicalDeviceDescriptorIndexingProperties dip = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
      VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      pp2.pNext = &dip;

      if (auto get2 = vk_.vulkan_instance()->functions().vkGetPhysicalDeviceProperties2)
      {
        get2(vk_.physical_device(), &pp2);

        if (dip.maxPerStageDescriptorUpdateAfterBindSampledImages && 3ull * srv_count_ > dip.maxPerStageDescriptorUpdateAfterBindSampledImages)
          REXLOG_WARN("[sst-native] Vulkan: {} texture descriptors per stage needed, the GPU allows {}", 3ull * srv_count_, dip.maxPerStageDescriptorUpdateAfterBindSampledImages);
      }

      const auto &p = vk_.properties();
      VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

      if (p.descriptorBindingUpdateUnusedWhilePending)
        flags |= VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

      for (uint32_t i = 0; i < kSets; ++i)
      {
        VkDescriptorSetLayoutBinding b = {};
        b.binding = 0;
        b.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        b.descriptorCount = i < 3 ? srv_count_ : sampler_count_;
        b.stageFlags = kStages;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 1;
        bf.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.pNext = &bf;
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        li.bindingCount = 1;
        li.pBindings = &b;

        if (fn_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layouts_[i]) != VK_SUCCESS)
        {
          REXLOG_ERROR("[sst-native] Vulkan: descriptor set layout {} failed", i);
          return false;
        }
      }

      const VkDescriptorPoolSize sizes_[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * srv_count_}, {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_count_}};
      VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
      pi.maxSets = kSets;
      pi.poolSizeCount = 2;
      pi.pPoolSizes = sizes_;

      if (fn_.vkCreateDescriptorPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS)
      {
        REXLOG_ERROR("[sst-native] Vulkan: descriptor pool ({} textures, {} samplers) failed", 3 * srv_count_, sampler_count_);
        return false;
      }

      const uint32_t counts[kSets] = {srv_count_, srv_count_, srv_count_, sampler_count_};
      VkDescriptorSetVariableDescriptorCountAllocateInfo vc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
      vc.descriptorSetCount = kSets;
      vc.pDescriptorCounts = counts;
      VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.pNext = &vc;
      ai.descriptorPool = pool_;
      ai.descriptorSetCount = kSets;
      ai.pSetLayouts = set_layouts_;

      if (fn_.vkAllocateDescriptorSets(device_, &ai, sets_) != VK_SUCCESS)
      {
        REXLOG_ERROR("[sst-native] Vulkan: descriptor sets failed");
        sets_[0] = VK_NULL_HANDLE;
        return false;
      }

      // The blits' linear clamp sampler
      D3D12_SAMPLER_DESC linear = {};
      linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      linear.MaxAnisotropy = 1;
      linear.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      linear.MaxLOD = D3D12_FLOAT32_MAX;
      WriteSampler(blit_sampler_slot_, linear);
      return true;
    }

    void Dev::WriteSrv(uint32_t slot, Texture &texture, const D3D12_SHADER_RESOURCE_VIEW_DESC &desc)
    {
      auto &t = static_cast<VulkanTexture &>(texture);

      if (slot >= srv_count_ || !t.image)
        return;

      VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = t.image;
      VkFormat format = desc.Format == DXGI_FORMAT_UNKNOWN ? t.format : Format(desc.Format).vk;

      if (format != t.format)
      {
        // Views keep the image's format (images aren't created mutable); typeless depth views read the depth plane
        if (!IsDepth(t.format))
        {
          std::lock_guard lock(tables_mutex_);

          if (WarnOnce((uint64_t(t.format) << 32) | uint32_t(desc.Format)))
            REXLOG_WARN("[sst-native] Vulkan: SRV format DXGI {} on a VkFormat {} image: using the image's format", uint32_t(desc.Format), uint32_t(t.format));
        }

        format = t.format;
      }

      vi.format = format;
      vi.components = Components(desc.Shader4ComponentMapping);
      VkImageAspectFlags aspect = t.aspect;

      if (IsDepth(t.format))
        aspect = desc.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || desc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;

      auto mip_range = [&](UINT most_detailed, UINT levels)
      {
        const uint32_t base = std::min<uint32_t>(most_detailed, t.mips - 1);
        const uint32_t count = levels == UINT(-1) || levels == 0 ? t.mips - base : std::min<uint32_t>(levels, t.mips - base);
        return std::pair<uint32_t, uint32_t>(base, count);
      };

      uint32_t set = 0, base_mip = 0, mip_count = 1, base_layer = 0, layer_count = 1;

      switch (desc.ViewDimension)
      {
      case D3D12_SRV_DIMENSION_TEXTURE2D:
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        std::tie(base_mip, mip_count) = mip_range(desc.Texture2D.MostDetailedMip, desc.Texture2D.MipLevels);
        break;

      case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        std::tie(base_mip, mip_count) = mip_range(desc.Texture2DArray.MostDetailedMip, desc.Texture2DArray.MipLevels);
        base_layer = std::min(desc.Texture2DArray.FirstArraySlice, t.layers - 1);
        layer_count = std::min(std::max(desc.Texture2DArray.ArraySize, 1u), t.layers - base_layer);
        break;

      case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        break;

      case D3D12_SRV_DIMENSION_TEXTURE3D:
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        set = 1;
        std::tie(base_mip, mip_count) = mip_range(desc.Texture3D.MostDetailedMip, desc.Texture3D.MipLevels);
        break;

      case D3D12_SRV_DIMENSION_TEXTURECUBE:
        if (!t.cube_compatible)
        {
          std::lock_guard lock(tables_mutex_);

          if (WarnOnce(0xC0BE000000000000ull | (uint64_t(t.extent.width) << 16) | t.layers))
            REXLOG_WARN("[sst-native] Vulkan: cube view of a {}x{}x{} image that can't be a cube", t.extent.width, t.extent.height, t.layers);

          return;
        }

        vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        set = 2;
        std::tie(base_mip, mip_count) = mip_range(desc.TextureCube.MostDetailedMip, desc.TextureCube.MipLevels);
        layer_count = 6;
        break;

      default:
        return;
      }

      if ((vi.viewType == VK_IMAGE_VIEW_TYPE_3D) != t.volume)
        return; // D3D12 doesn't allow it either

      vi.subresourceRange = {aspect, base_mip, mip_count, base_layer, layer_count};
      const VkImageView view = t.AddView(vi);

      if (!view)
        return;

      VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets_[set];
      w.dstBinding = 0;
      w.dstArrayElement = slot;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w.pImageInfo = &ii;
      std::lock_guard lock(tables_mutex_);
      fn_.vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    void Dev::WriteSampler(uint32_t slot, const D3D12_SAMPLER_DESC &desc)
    {
      if (slot >= sampler_count_)
        return;

      const auto &p = vk_.properties();
      const uint32_t f = uint32_t(desc.Filter);
      const bool aniso = (f & 0x40) != 0;
      const uint32_t reduction = (f >> 7) & 3;
      VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      si.minFilter = ((f >> 4) & 1) || aniso ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      si.magFilter = ((f >> 2) & 1) || aniso ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      si.mipmapMode = (f & 1) || aniso ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
      si.addressModeU = AddressMode(desc.AddressU, p.samplerMirrorClampToEdge);
      si.addressModeV = AddressMode(desc.AddressV, p.samplerMirrorClampToEdge);
      si.addressModeW = AddressMode(desc.AddressW, p.samplerMirrorClampToEdge);
      const float max_bias = physical_properties_.limits.maxSamplerLodBias;
      si.mipLodBias = std::clamp(desc.MipLODBias, -max_bias, max_bias);
      si.anisotropyEnable = aniso && p.samplerAnisotropy && desc.MaxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
      si.maxAnisotropy = si.anisotropyEnable ? std::clamp(float(desc.MaxAnisotropy), 1.0f, p.maxSamplerAnisotropy) : 1.0f;
      si.compareEnable = reduction == 1 ? VK_TRUE : VK_FALSE;
      si.compareOp = CompareOp(desc.ComparisonFunc);
      si.minLod = desc.MinLOD;
      si.maxLod = desc.MaxLOD >= 1000.0f ? VK_LOD_CLAMP_NONE : desc.MaxLOD;
      const float *bc = desc.BorderColor;

      // Vulkan has three fixed border colours
      si.borderColor = bc[0] >= 0.5f && bc[1] >= 0.5f && bc[2] >= 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : bc[3] >= 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                                                                                            : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

      VkSampler sampler = VK_NULL_HANDLE;

      if (fn_.vkCreateSampler(device_, &si, nullptr, &sampler) != VK_SUCCESS)
        return;

      VkDescriptorImageInfo ii = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets_[3];
      w.dstBinding = 0;
      w.dstArrayElement = slot;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w.pImageInfo = &ii;
      VkSampler old;

      {
        std::lock_guard lock(tables_mutex_);
        fn_.vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        old = samplers_[slot];
        samplers_[slot] = sampler;
      }

      if (old)
      {
        Retire([this, old]
               { fn_.vkDestroySampler(device_, old, nullptr); });
      }
    }

    void Dev::WriteRtv(uint32_t slot, Texture &texture, const D3D12_RENDER_TARGET_VIEW_DESC *desc)
    {
      auto &t = static_cast<VulkanTexture &>(texture);

      if (slot >= rtvs_.size() || !t.image)
        return;

      uint32_t mip = 0, layer = 0;

      if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D)
      {
        mip = desc->Texture3D.MipSlice;
        layer = std::min(desc->Texture3D.FirstWSlice, std::max(t.extent.depth >> mip, 1u) - 1);
      }
      else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY)
      {
        mip = desc->Texture2DArray.MipSlice;
        layer = desc->Texture2DArray.FirstArraySlice;
      }
      else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D)
      {
        mip = desc->Texture2D.MipSlice;
      }

      mip = std::min(mip, t.mips - 1);
      VkImageViewUsageCreateInfo usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // (a 2D view of a 3D image may only be an attachment)
      VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.pNext = &usage;
      vi.image = t.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = t.format;
      vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
      const VkImageView view = t.AddView(vi);
      std::lock_guard lock(tables_mutex_);
      rtvs_[slot] = {view, {std::max(t.extent.width >> mip, 1u), std::max(t.extent.height >> mip, 1u)}, t.format, VK_IMAGE_ASPECT_COLOR_BIT};
    }

    void Dev::WriteDsv(uint32_t slot, Texture &texture, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
    {
      auto &t = static_cast<VulkanTexture &>(texture);

      if (slot >= dsvs_.size() || !t.image || !IsDepth(t.format))
        return;

      const uint32_t mip = desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ? std::min(desc->Texture2D.MipSlice, t.mips - 1) : 0;
      VkImageViewUsageCreateInfo usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      usage.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.pNext = &usage;
      vi.image = t.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = t.format;
      vi.subresourceRange = {t.aspect, mip, 1, 0, 1};
      const VkImageView view = t.AddView(vi);
      std::lock_guard lock(tables_mutex_);
      dsvs_[slot] = {view, {std::max(t.extent.width >> mip, 1u), std::max(t.extent.height >> mip, 1u)}, t.format, t.aspect};
    }

    bool Dev::CreateLayouts()
    {
      if (!has_sets())
        return false;

      const VkPushConstantRange range = {kStages, 0, kPushBytes};
      VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      li.setLayoutCount = kSets;
      li.pSetLayouts = set_layouts_;
      li.pushConstantRangeCount = 1;
      li.pPushConstantRanges = &range;
      return fn_.vkCreatePipelineLayout(device_, &li, nullptr, &layout_) == VK_SUCCESS;
    }

    VkShaderModule Dev::Module(const ShaderCode &code)
    {
      VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      std::vector<uint32_t> aligned;
      mi.codeSize = code.size & ~size_t(3);

      if (reinterpret_cast<uintptr_t>(code.data) & 3)
      {
        aligned.resize(mi.codeSize / 4);
        std::memcpy(aligned.data(), code.data, mi.codeSize);
        mi.pCode = aligned.data();
      }
      else
      {
        mi.pCode = reinterpret_cast<const uint32_t *>(code.data);
      }

      VkShaderModule m = VK_NULL_HANDLE;
      return fn_.vkCreateShaderModule(device_, &mi, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
    }

    PipelineRef Dev::BuildPipeline(const PipelineDesc &desc, VkPipelineCache cache, int32_t *error)
    {
      auto fail = [&](VkResult r) -> PipelineRef
      {
        if (error)
          *error = int32_t(r);

        return nullptr;
      };

      if (!layout_ || !desc.vs)
        return fail(VK_ERROR_INITIALIZATION_FAILED);

      const VkShaderModule vs = Module(desc.vs);
      const VkShaderModule ps = desc.ps ? Module(desc.ps) : VK_NULL_HANDLE;

      struct Modules
      {
        const Fn &fn;
        VkDevice device;
        VkShaderModule a, b;

        ~Modules()
        {
          if (a)
            fn.vkDestroyShaderModule(device, a, nullptr);

          if (b)
            fn.vkDestroyShaderModule(device, b, nullptr);
        }
      } modules{fn_, device_, vs, ps};

      if (!vs || (desc.ps && !ps))
        return fail(VK_ERROR_INVALID_SHADER_NV);

      // Specialization constant 0: the variant (g_SpecConstants)
      const VkSpecializationMapEntry entry = {0, 0, 4};
      const VkSpecializationInfo spec_vs = {1, &entry, 4, &desc.vs.spec};
      const VkSpecializationInfo spec_ps = {1, &entry, 4, &desc.ps.spec};
      VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = vs;
      stages[0].pName = "main";
      stages[0].pSpecializationInfo = &spec_vs;
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = ps;
      stages[1].pName = "main";
      stages[1].pSpecializationInfo = &spec_ps;

      // Vertex input: D3D12 slots -> bindings (strides are dynamic)
      std::vector<VkVertexInputBindingDescription> bindings;
      std::vector<VkVertexInputAttributeDescription> attributes;
      std::unordered_map<uint32_t, uint32_t> next_offset; // D3D12_APPEND_ALIGNED_ELEMENT

      for (const InputElement &e : desc.inputs)
      {
        const uint32_t binding = Binding(e.d3d.InputSlot);
        const VkVertexInputRate rate = e.d3d.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;

        if (std::none_of(bindings.begin(), bindings.end(), [&](const auto &b)
                         { return b.binding == binding; }))
          bindings.push_back({binding, 0, rate});

        const FormatInfo fi = Format(e.d3d.Format);
        uint32_t &next = next_offset[binding];
        const uint32_t offset = e.d3d.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? next : e.d3d.AlignedByteOffset;
        next = offset + fi.bytes;
        attributes.push_back({e.location, binding, fi.vk, offset});
      }

      VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
      vi.pVertexBindingDescriptions = bindings.data();
      vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
      vi.pVertexAttributeDescriptions = attributes.data();

      VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = desc.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : desc.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                                                                                                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

      VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = 1;
      vp.scissorCount = 1;

      const auto &p = vk_.properties();
      const D3D12_RASTERIZER_DESC &r = desc.raster;
      VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.depthClampEnable = !r.DepthClipEnable && p.depthClamp ? VK_TRUE : VK_FALSE;
      rs.polygonMode = r.FillMode == D3D12_FILL_MODE_WIREFRAME && p.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
      rs.cullMode = r.CullMode == D3D12_CULL_MODE_FRONT ? VK_CULL_MODE_FRONT_BIT : r.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT
                                                                                                                      : VK_CULL_MODE_NONE;

      // (-fvk-invert-y keeps the screen-space winding D3D12 sees)
      rs.frontFace = r.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
      rs.depthBiasEnable = r.DepthBias != 0 || r.SlopeScaledDepthBias != 0 ? VK_TRUE : VK_FALSE;
      rs.depthBiasConstantFactor = float(r.DepthBias);
      rs.depthBiasClamp = r.DepthBiasClamp;
      rs.depthBiasSlopeFactor = r.SlopeScaledDepthBias;
      rs.lineWidth = 1.0f;

      VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VkSampleCountFlagBits(std::max(desc.samples, 1u));
      ms.alphaToCoverageEnable = desc.blend.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;

      const D3D12_DEPTH_STENCIL_DESC &d = desc.depth;
      VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      ds.depthTestEnable = d.DepthEnable ? VK_TRUE : VK_FALSE;
      ds.depthWriteEnable = d.DepthEnable && d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? VK_TRUE : VK_FALSE;
      ds.depthCompareOp = CompareOp(d.DepthFunc);
      ds.stencilTestEnable = d.StencilEnable ? VK_TRUE : VK_FALSE;

      auto stencil = [&](const D3D12_DEPTH_STENCILOP_DESC &o)
      {
        VkStencilOpState s = {};
        s.failOp = StencilOp(o.StencilFailOp);
        s.passOp = StencilOp(o.StencilPassOp);
        s.depthFailOp = StencilOp(o.StencilDepthFailOp);
        s.compareOp = CompareOp(o.StencilFunc);
        s.compareMask = d.StencilReadMask;
        s.writeMask = d.StencilWriteMask;
        s.reference = 0;
        return s;
      };

      ds.front = stencil(d.FrontFace);
      ds.back = stencil(d.BackFace);

      const bool color = desc.rtv != DXGI_FORMAT_UNKNOWN;
      const D3D12_RENDER_TARGET_BLEND_DESC &b = desc.blend.RenderTarget[0];
      VkPipelineColorBlendAttachmentState att = {};
      att.blendEnable = b.BlendEnable ? VK_TRUE : VK_FALSE;
      att.srcColorBlendFactor = BlendFactor(b.SrcBlend);
      att.dstColorBlendFactor = BlendFactor(b.DestBlend);
      att.colorBlendOp = BlendOp(b.BlendOp);
      att.srcAlphaBlendFactor = BlendFactor(b.SrcBlendAlpha);
      att.dstAlphaBlendFactor = BlendFactor(b.DestBlendAlpha);
      att.alphaBlendOp = BlendOp(b.BlendOpAlpha);
      att.colorWriteMask = b.RenderTargetWriteMask & 0xF;
      VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = color ? 1 : 0;
      cb.pAttachments = &att;

      // (The vertex strides only with vertex input: Vulkan wants them bound even for a pipeline with no vertex buffers)
      const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
      VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dy.dynamicStateCount = uint32_t(std::size(dynamic)) - (bindings.empty() ? 1 : 0);
      dy.pDynamicStates = dynamic;

      const VkFormat color_format = color ? Format(desc.rtv).vk : VK_FORMAT_UNDEFINED;
      const VkFormat depth_format = desc.dsv != DXGI_FORMAT_UNKNOWN ? Format(desc.dsv).vk : VK_FORMAT_UNDEFINED;
      VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      ri.colorAttachmentCount = color ? 1 : 0;
      ri.pColorAttachmentFormats = &color_format;
      ri.depthAttachmentFormat = IsDepth(depth_format) ? depth_format : VK_FORMAT_UNDEFINED;
      ri.stencilAttachmentFormat = HasStencil(depth_format) ? depth_format : VK_FORMAT_UNDEFINED;

      VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pi.pNext = &ri;
      pi.stageCount = ps ? 2 : 1;
      pi.pStages = stages;
      pi.pVertexInputState = &vi;
      pi.pInputAssemblyState = &ia;
      pi.pViewportState = &vp;
      pi.pRasterizationState = &rs;
      pi.pMultisampleState = &ms;
      pi.pDepthStencilState = &ds;
      pi.pColorBlendState = &cb;
      pi.pDynamicState = &dy;
      pi.layout = layout_;

      auto pipeline = std::make_shared<VulkanPipeline>(*this);
      const VkResult result = fn_.vkCreateGraphicsPipelines(device_, cache, 1, &pi, nullptr, &pipeline->pipeline);

      if (error)
        *error = int32_t(result);

      if (result != VK_SUCCESS)
      {
        pipeline->pipeline = VK_NULL_HANDLE;
        return nullptr;
      }

      pipeline->strip_cut = desc.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      pipeline->vertex_input = !bindings.empty();
      return pipeline;
    }

    std::unique_ptr<PipelineCache> Dev::OpenPipelineCache(const std::filesystem::path &path)
    {
      // Next to the D3D12 one: pipelines.bin -> pipelines.vulkan.bin
      std::filesystem::path vk_path = path;
      vk_path.replace_extension(".vulkan.bin");
      auto cache = std::make_unique<VulkanPipelineCache>(*this, vk_path);
      return cache->Open() ? std::move(cache) : nullptr;
    }

    TimestampPoolRef Dev::CreateTimestampPool(uint32_t count)
    {
      if (!timestamp_bits_ || !count)
        return nullptr;

      VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = count;
      auto pool = std::make_shared<VulkanTimestampPool>(*this);

      if (fn_.vkCreateQueryPool(device_, &qi, nullptr, &pool->pool) != VK_SUCCESS)
      {
        pool->pool = VK_NULL_HANDLE;
        return nullptr;
      }

      pool->count = count;
      auto lock = SetupLock();

      if (VkCommandBuffer cb = SetupCommands())
        fn_.vkCmdResetQueryPool(cb, pool->pool, 0, count);

      return pool;
    }

    std::unique_ptr<CommandList> Dev::CreateCommandList(uint32_t slots)
    {
      auto list = std::make_unique<VulkanCommandList>(*this);
      return list->Create(slots) ? std::move(list) : nullptr;
    }

    uint64_t Dev::Submit(CommandList &list)
    {
      const uint64_t value = submitted_.load(std::memory_order_relaxed) + 1;
      VkCommandBuffer cbs[2];
      uint32_t n = 0;

      {
        auto lock = SetupLock();

        if (setup_.cb)
        {
          fn_.vkEndCommandBuffer(setup_.cb);
          cbs[n++] = setup_.cb;
          setup_.fence = value;
          setup_done_.push_back(setup_);
          setup_ = {};
        }
      }

      cbs[n++] = static_cast<VulkanCommandList &>(list).cb();
      VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      ts.signalSemaphoreValueCount = 1;
      ts.pSignalSemaphoreValues = &value;
      VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.pNext = &ts;
      si.commandBufferCount = n;
      si.pCommandBuffers = cbs;
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &timeline_;
      VkResult result;

      {
        auto queue = vk_.AcquireQueue(family_, 0);
        result = fn_.vkQueueSubmit(queue.queue(), 1, &si, VK_NULL_HANDLE);
      }

      if (result != VK_SUCCESS)
        REXLOG_ERROR("[sst-native] Vulkan: vkQueueSubmit failed ({})", int32_t(result));

      submitted_.store(value, std::memory_order_release);
      Collect();
      return value;
    }

    uint64_t Dev::CompletedFence()
    {
      uint64_t value = 0;
      fn_.vkGetSemaphoreCounterValue(device_, timeline_, &value);
      return value;
    }

    void Dev::WaitFence(uint64_t value)
    {
      if (!value || CompletedFence() >= value)
        return;

      VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wi.semaphoreCount = 1;
      wi.pSemaphores = &timeline_;
      wi.pValues = &value;
      fn_.vkWaitSemaphores(device_, &wi, UINT64_MAX);
    }
  }

  std::unique_ptr<Device> CreateDevice(const VulkanProvider &provider)
  {
    if (!provider.vulkan_device())
      return nullptr;

    auto device = std::make_unique<Dev>(provider);
    return device->Initialize() ? std::move(device) : nullptr;
  }

  uint64_t NativeImage(Texture &texture)
  {
    auto *t = dynamic_cast<VulkanTexture *>(&texture);
    return t ? uint64_t(reinterpret_cast<uintptr_t>(t->image)) : 0;
  }
}