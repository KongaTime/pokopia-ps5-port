// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio output for the runtime's audio services (nxrt/audio_sink.h): sceAudioOut, one main port, 48 kHz 16-bit
// stereo in grains of 256 frames, as KongaTime's KH 358/2 Days port opens it (ps5/sdl/audio_ps5.c). write() fills a ring; a thread
// hands the port a grain at a time, and sceAudioOutOutput blocks until the device takes it, so the ring drains at the device's
// own rate and queued_frames() is a real device clock (the renderer paces itself by it). 5.1 from the game is mixed down to
// stereo; another rate than 48 kHz is refused (the services resample to 48 kHz).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutOutput(int port, const void* samples);
int sceAudioOutClose(int port);
}

namespace nxrt::audio {
namespace {

constexpr uint32_t kRate = 48000, kGrain = 256;
constexpr uint32_t kRingFrames = kRate / 2;   // 500 ms

class Ps5Sink : public AudioSink {
public:
    ~Ps5Sink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override {
        close();
        if (sample_rate != kRate || (channels != 2 && channels != 6)) {
            NXLOG_W("audio", "ps5 sink: %u Hz %u channels not supported (48000 Hz, 2 or 6 channels)", sample_rate, channels);
            return false;
        }
        const int init = sceAudioOutInit();
        if (init < 0 && init != static_cast<int>(0x8026000e)) {   // already initialised
            NXLOG_W("audio", "ps5 sink: sceAudioOutInit failed: 0x%08x", static_cast<unsigned>(init));
            return false;
        }
        port_ = sceAudioOutOpen(0xff, 0, 0, kGrain, kRate, 1);   // system user, main port, S16 stereo
        if (port_ < 0) {
            NXLOG_W("audio", "ps5 sink: sceAudioOutOpen failed: 0x%08x", static_cast<unsigned>(port_));
            port_ = -1;
            return false;
        }
        channels_ = channels;
        ring_.assign(static_cast<size_t>(kRingFrames) * 2, 0);
        head_ = tail_ = 0;
        stop_ = false;
        stats_ = SinkStats{};
        stats_.sample_rate = kRate;
        thread_ = std::thread([this] { run(); });
        NXLOG_I("audio", "ps5 sink: sceAudioOut port %d open at %u Hz, %u channels in (stereo out), grains of %u frames", port_, kRate,
                channels, kGrain);
        return true;
    }

    void close() override {
        if (port_ < 0) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        sceAudioOutOutput(port_, nullptr);   // waits for the last grain
        sceAudioOutClose(port_);
        port_ = -1;
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.frames_written += frames;
        const uint32_t room = kRingFrames - 1 - fill();
        const uint32_t n = std::min(frames, room);
        for (uint32_t i = 0; i < n; ++i) {
            int16_t l, r;
            if (channels_ == 2) {
                l = pcm[i * 2];
                r = pcm[i * 2 + 1];
            } else {   // FL FR FC LFE BL BR -> stereo
                const int16_t* f = pcm + i * 6;
                const int c = f[2] * 7 / 10, s = 7;   // centre at -3 dB, surround at -3 dB
                l = static_cast<int16_t>(std::clamp(f[0] + c + f[4] * s / 10, -32768, 32767));
                r = static_cast<int16_t>(std::clamp(f[1] + c + f[5] * s / 10, -32768, 32767));
            }
            ring_[head_ * 2] = l;
            ring_[head_ * 2 + 1] = r;
            head_ = (head_ + 1) % kRingFrames;
        }
        stats_.frames_accepted += n;
        stats_.frames_dropped += frames - n;
        stats_.queued_peak_frames = std::max(stats_.queued_peak_frames, fill());
        cv_.notify_one();
        return n;
    }

    uint32_t queued_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        return fill();
    }
    bool has_device_clock() const override { return true; }
    const char* name() const override { return "ps5"; }
    SinkStats stats() override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

private:
    uint32_t fill() const { return (head_ + kRingFrames - tail_) % kRingFrames; }

    void run() {
        std::vector<int16_t> grain(kGrain * 2);
        bool started = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                if (stop_) return;
                const uint32_t have = fill();
                const uint32_t n = std::min(have, kGrain);
                for (uint32_t i = 0; i < n; ++i) {
                    grain[i * 2] = ring_[tail_ * 2];
                    grain[i * 2 + 1] = ring_[tail_ * 2 + 1];
                    tail_ = (tail_ + 1) % kRingFrames;
                }
                if (n < kGrain) {   // ran dry: silence for the rest of the grain (the device keeps its clock)
                    std::fill(grain.begin() + n * 2, grain.end(), int16_t(0));
                    if (started && n == 0) ++stats_.underruns;
                }
                if (n) started = true;
                if (started) stats_.queued_low_frames = std::min(stats_.queued_low_frames ? stats_.queued_low_frames : ~0u, fill());
            }
            sceAudioOutOutput(port_, grain.data());   // blocks until the device takes the grain
        }
    }

    int port_ = -1;
    uint32_t channels_ = 2;
    std::vector<int16_t> ring_;
    uint32_t head_ = 0, tail_ = 0;
    bool stop_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    SinkStats stats_;
};

}  // namespace

// The console's sink where the PC build makes its XAudio2 one ([audio] sink = auto takes it when open() succeeds).
std::shared_ptr<AudioSink> make_xaudio2_sink() { return std::make_shared<Ps5Sink>(); }

}  // namespace nxrt::audio
