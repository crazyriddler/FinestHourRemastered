// the mixer pulled every 10 ms by the clock, its output dropped
#include <chrono>
#include <thread>
#include <vector>

#include "audio/audio.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render, uint32_t channels) {
    std::thread([render, channels] {
        std::vector<float> b(kRate / 100 * channels);
        for (;;) {
            render(b.data(), kRate / 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

}  // namespace audio
