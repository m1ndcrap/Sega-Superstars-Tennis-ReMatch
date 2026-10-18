// The SDK's SDL audio backend fails to create its driver when Windows has no playback device (or its driver is broken): the game's XAudioRegisterRenderDriverClient then fails and the game crashes at boot (a guest null read in sub_8215CDE8). This backend is the SDK's SDL one, but when the device
// cannot be opened it hands the game a silent driver instead: it takes the game's audio frames and drops them, so the game's audio thread runs as it would with sound.

#pragma once

#include <memory>

namespace rex::runtime
{
  class FunctionDispatcher;
}

namespace rex::system
{
  class IAudioSystem;
}

namespace sst::audio
{
  // For RuntimeConfig::audio_factory
  std::unique_ptr<rex::system::IAudioSystem> CreateAudioSystem(rex::runtime::FunctionDispatcher *dispatcher);
}