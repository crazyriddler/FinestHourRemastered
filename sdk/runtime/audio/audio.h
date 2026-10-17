#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// the mix as it goes out, to a recording (null: none): interleaved stereo float at kRate, on the audio thread
void SetTap(std::function<void(const float* mix, uint32_t frames)> tap);

}  // namespace audio
