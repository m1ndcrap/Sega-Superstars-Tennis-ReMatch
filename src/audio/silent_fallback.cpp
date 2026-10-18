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
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#endif

namespace sst::audio
{
  using rex::X_STATUS;

  namespace
  {
    class AudioSystem;

    // Whether the system has a playback device now (Windows: an active render endpoint; elsewhere: unknown, so "maybe")
    class DeviceWatch
    {
    public:
      DeviceWatch()
      {
#if defined(_WIN32)
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator_));
#endif
      }

      ~DeviceWatch()
      {
#if defined(_WIN32)
        if (enumerator_)
          enumerator_->Release();

        if (com_)
          CoUninitialize();
#endif
      }

      bool Present()
      {
#if defined(_WIN32)
        if (!enumerator_)
          return true;

        IMMDeviceCollection *devices = nullptr;
        UINT count = 0;

        if (SUCCEEDED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) && devices)
        {
          devices->GetCount(&count);
          devices->Release();
        }

        return count > 0;
#else
        return true;
#endif
      }

    private:
#if defined(_WIN32)
      IMMDeviceEnumerator *enumerator_ = nullptr;
      bool com_ = false;
#endif
    };

    // Stands in while there is no audio device: takes the game's frames and lets it go on at the pace a sound card would. Meanwhile it watches for a playback device; when one appears it opens it (the SDK's SDL driver) and hands it the game's sound from then on.
    class FallbackDriver final : public rex::audio::AudioDriver
    {
    public:
      FallbackDriver(AudioSystem *system, rex::memory::Memory *memory, size_t index, rex::thread::Semaphore *semaphore) : AudioDriver(memory), system_(system), index_(index), semaphore_(semaphore)
      {
        thread_ = std::thread([this]
                              { Run(); });
      }

      ~FallbackDriver() override;

      void SubmitFrame(uint32_t frame) override
      {
        std::lock_guard lock(mutex_);

        if (real_)
          real_->SubmitFrame(frame);
        else
          ++pending_;
      }

    private:
      static constexpr std::chrono::nanoseconds kFrame{256'000'000'000LL / 48000}; // 5.333 ms
      static constexpr std::chrono::seconds kWatch{1}, kRetry{5};

      void Run();

      AudioSystem *system_;
      size_t index_;
      rex::thread::Semaphore *semaphore_;
      std::mutex mutex_;
      rex::audio::AudioDriver *real_ = nullptr; // the device's driver, once there is one (under mutex_)
      int pending_ = 0;                         // frames taken and not yet "played" (under mutex_)
      std::atomic<bool> running_{true};
      std::thread thread_;
    };

    // The SDK's SDL backend, with the fallback driver when the device can't be opened
    class AudioSystem final : public rex::audio::sdl::SDLAudioSystem
    {
    public:
      using SDLAudioSystem::SDLAudioSystem;

      X_STATUS CreateDriver(size_t index, rex::thread::Semaphore *semaphore, rex::audio::AudioDriver **out_driver) override
      {
        const X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);

        if (XSUCCEEDED(status))
          return status;

        REXLOG_WARN("[sst-audio] no audio device could be opened (status {:08X}): no sound until one is connected", uint32_t(status));
        *out_driver = new FallbackDriver(this, memory_, index, semaphore);
        return X_STATUS_SUCCESS;
      }

      void DestroyDriver(rex::audio::AudioDriver *driver) override
      {
        if (auto *fallback = dynamic_cast<FallbackDriver *>(driver))
        {
          delete fallback;
          return;
        }

        SDLAudioSystem::DestroyDriver(driver);
      }

      // The SDK's driver for the device (null when it can't be opened)
      rex::audio::AudioDriver *OpenDevice(size_t index, rex::thread::Semaphore *semaphore)
      {
        rex::audio::AudioDriver *driver = nullptr;
        return XSUCCEEDED(SDLAudioSystem::CreateDriver(index, semaphore, &driver)) ? driver : nullptr;
      }

      void CloseDevice(rex::audio::AudioDriver *driver) { SDLAudioSystem::DestroyDriver(driver); }
    };

    FallbackDriver::~FallbackDriver()
    {
      running_ = false;

      if (thread_.joinable())
        thread_.join();

      if (real_)
        system_->CloseDevice(real_);
    }

    void FallbackDriver::Run()
    {
      using clock = std::chrono::steady_clock;
      DeviceWatch watch;
      auto start = clock::now(), next_watch = start + kWatch;
      uint64_t played = 0;

      while (running_)
      {
        const auto now = clock::now();

        // Frames whose playing time has passed: let the game make that many more (no more than it has handed over; no catching up)
        {
          std::lock_guard lock(mutex_);
          const uint64_t due = uint64_t((now - start) / kFrame);
          const uint64_t n = std::min<uint64_t>(due - std::min(due, played), uint64_t(pending_));

          if (n)
          {
            pending_ -= int(n);
            semaphore_->Release(int(n), nullptr);
          }

          played = due;
        }

        // A playback device appeared: open it and send the sound there from now on
        if (now >= next_watch)
        {
          next_watch = now + kWatch;

          if (watch.Present())
          {
            if (rex::audio::AudioDriver *real = system_->OpenDevice(index_, semaphore_))
            {
              std::lock_guard lock(mutex_);
              real_ = real;

              // (frames taken while silent: done)
              if (pending_)
                semaphore_->Release(pending_, nullptr);

              pending_ = 0;
              REXLOG_INFO("[sst-audio] audio device connected: sound on");
              return;
            }

            next_watch = now + kRetry; // (listed but not usable yet)
          }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }

  std::unique_ptr<rex::system::IAudioSystem> CreateAudioSystem(rex::runtime::FunctionDispatcher *dispatcher)
  {
    return std::make_unique<AudioSystem>(dispatcher);
  }
}