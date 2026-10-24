#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved float frames from render: 2 channels (left, right) or 6 (5.1: front left,
// front right, centre, subwoofer, back left, back right)
void StartOutput(std::function<void(float* out, uint32_t frames)> render, uint32_t channels = 2);

// the mix as it goes out, to a recording (null: none): interleaved stereo float at kRate (5.1 folded down), on the
// audio thread
void SetTap(std::function<void(const float* mix, uint32_t frames)> tap);

}  // namespace audio
