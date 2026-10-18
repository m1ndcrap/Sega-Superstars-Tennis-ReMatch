// Audio without an audio device

#include "audio/silent_fallback.h"

#include <rex/audio/audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace sst::audio
{
  using rex::X_STATUS;

  namespace
  {
    // Takes the game's frames and lets it go on at the pace a sound card would: one frame (256 samples at 48 kHz) every 5.33 ms
    class SilentDriver final : public rex::audio::AudioDriver
    {
    public:
      SilentDriver(rex::memory::Memory *memory, rex::thread::Semaphore *semaphore) : AudioDriver(memory), semaphore_(semaphore)
      {
        thread_ = std::thread([this]
                              { Run(); });
      }

      ~SilentDriver() override
      {
        running_ = false;

        if (thread_.joinable())
          thread_.join();
      }

      void SubmitFrame(uint32_t) override { pending_.fetch_add(1); }

    private:
      static constexpr std::chrono::nanoseconds kFrame{256'000'000'000LL / 48000}; // 5.333 ms

      void Run()
      {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        uint64_t played = 0;

        while (running_)
        {
          // Frames whose playing time has passed: let the game make that many more (no more than it has handed over)
          const uint64_t due = uint64_t((clock::now() - start) / kFrame);

          while (played < due)
          {
            int pending = pending_.load();

            if (pending <= 0)
            {
              played = due; // (the game was late: no catching up with a burst)
              break;
            }

            if (pending_.compare_exchange_weak(pending, pending - 1))
            {
              semaphore_->Release(1, nullptr);
              ++played;
            }
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }

      rex::thread::Semaphore *semaphore_;
      std::atomic<int> pending_{0};
      std::atomic<bool> running_{true};
      std::thread thread_;
    };

    // The SDK's SDL backend, with the silent driver when the device can't be opened
    class AudioSystem final : public rex::audio::sdl::SDLAudioSystem
    {
    public:
      using SDLAudioSystem::SDLAudioSystem;

      X_STATUS CreateDriver(size_t index, rex::thread::Semaphore *semaphore, rex::audio::AudioDriver **out_driver) override
      {
        const X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);

        if (XSUCCEEDED(status))
          return status;

        REXLOG_WARN("[sst-audio] no audio device could be opened (status {:08X}): the game runs without sound", uint32_t(status));
        *out_driver = new SilentDriver(memory_, semaphore);
        return X_STATUS_SUCCESS;
      }

      void DestroyDriver(rex::audio::AudioDriver *driver) override
      {
        if (auto *silent = dynamic_cast<SilentDriver *>(driver))
        {
          delete silent;
          return;
        }

        SDLAudioSystem::DestroyDriver(driver);
      }
    };
  }

  std::unique_ptr<rex::system::IAudioSystem> CreateAudioSystem(rex::runtime::FunctionDispatcher *dispatcher)
  {
    return std::make_unique<AudioSystem>(dispatcher);
  }
}