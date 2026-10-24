// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {
std::mutex g_tap_lock;
std::function<void(const float*, uint32_t)> g_tap;
}  // namespace

void SetTap(std::function<void(const float*, uint32_t)> tap) {
    std::lock_guard l(g_tap_lock);
    g_tap = std::move(tap);
}

void StartOutput(std::function<void(float*, uint32_t)> render, uint32_t channels) {
    channels = channels == 6 ? 6 : 2;
    std::thread([render, channels] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        // (5.1: the mastering voice takes the device's own speakers, XAudio2 folds the six down to fewer)
        WAVEFORMATEXTENSIBLE wx{};
        WAVEFORMATEX& wf = wx.Format;
        wf.wFormatTag = channels == 2 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_EXTENSIBLE;
        wf.nChannels = WORD(channels);
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = WORD(4 * channels);
        wf.nAvgBytesPerSec = kRate * 4 * channels;
        if (channels != 2) {
            wf.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wx.Samples.wValidBitsPerSample = 32;
            wx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            wx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xa->CreateMasteringVoice(&master)) ||
            FAILED(xa->CreateSourceVoice(&voice, &wf))) {
            LOGE("audio: XAudio2 unavailable, no sound");
            return;
        }
        voice->Start();
        {
            XAUDIO2_VOICE_DETAILS md{};
            master->GetVoiceDetails(&md);
            LOGR("audio: %u channel mix out to %u speakers", channels, md.InputChannels);
        }
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * channels), folded(kFrames * 2);
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= 3) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * channels];
            render(b, kFrames);
            // tests (XBE_AUDIO_LEVELS): each channel's loudness (RMS, dB) every 2 seconds
            static const bool levels = std::getenv("XBE_AUDIO_LEVELS") != nullptr;
            if (levels) {
                static double sq[6] = {};
                static uint32_t got = 0;
                for (uint32_t i = 0; i < kFrames * channels; i++) sq[i % channels] += double(b[i]) * b[i];
                if ((got += kFrames) >= kRate * 2) {
                    char text[128];
                    int k = std::snprintf(text, sizeof(text), "audio: levels");
                    for (uint32_t c = 0; c < channels; c++) {
                        k += std::snprintf(text + k, sizeof(text) - size_t(k), " %.1f", 10 * std::log10(sq[c] / got + 1e-12));
                        sq[c] = 0;
                    }
                    got = 0;
                    LOGR("%s", text);
                }
            }
            {
                std::lock_guard l(g_tap_lock);
                if (g_tap && channels == 2) {
                    g_tap(b, kFrames);
                } else if (g_tap) {
                    // the recording's stereo: the centre and each back channel at -3 dB on their side, no subwoofer
                    for (uint32_t i = 0; i < kFrames; i++) {
                        const float* f = b + i * 6;
                        folded[i * 2] = f[0] + 0.707f * (f[2] + f[4]);
                        folded[i * 2 + 1] = f[1] + 0.707f * (f[2] + f[5]);
                    }
                    g_tap(folded.data(), kFrames);
                }
            }
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 4 * channels;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
