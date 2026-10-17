#pragma once
#include <chrono>
#include <string>

// Tests: a sampling profiler of this process. About every millisecond each thread that ran since its last sample is
// stopped a moment and where it is noted (the function of this exe it is in, or that called out of it); the frame
// times' report (gpu::g_frame_stats, XBE_PERF_SAMPLE set) asks what ran in its worst frames.
namespace sampler {
void Start();
void Stop();
// the samples between two times, by thread and function, the most taken first: a line each, up to n
std::string Top(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to, int n);
// per thread busy over the span: its samples' innermost function (any module) and this exe's functions on the stack
// (inclusive), up to n each, from 0.5% up
std::string Profile(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to, int n);
}  // namespace sampler
