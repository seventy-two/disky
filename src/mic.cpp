#include "mic.h"

#include <cmath>
#include <mutex>

#include "miniaudio/miniaudio.h"
#include "rtp.h"

namespace disky {

namespace {

// findDevice looks a device up by name, filling id; false if it isn't there.
bool findDevice(ma_context* ctx, bool capture, const std::string& name, ma_device_id* id) {
    ma_device_info *play = nullptr, *cap = nullptr;
    ma_uint32 nPlay = 0, nCap = 0;
    if (ma_context_get_devices(ctx, &play, &nPlay, &cap, &nCap) != MA_SUCCESS) return false;
    ma_device_info* list = capture ? cap : play;
    ma_uint32 n = capture ? nCap : nPlay;
    for (ma_uint32 i = 0; i < n; i++)
        if (name == list[i].name) {
            *id = list[i].id;
            return true;
        }
    return false;
}

}  // namespace

std::vector<std::string> AudioDevices(bool capture) {
    std::vector<std::string> out;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return out;
    ma_device_info *play = nullptr, *cap = nullptr;
    ma_uint32 nPlay = 0, nCap = 0;
    if (ma_context_get_devices(&ctx, &play, &nPlay, &cap, &nCap) == MA_SUCCESS) {
        ma_device_info* list = capture ? cap : play;
        ma_uint32 n = capture ? nCap : nPlay;
        for (ma_uint32 i = 0; i < n; i++) out.push_back(list[i].name);
    }
    ma_context_uninit(&ctx);
    return out;
}

// OpenPlayback opens and starts the named output, or the default, for
// voice.cpp, in a context it's set up.
bool OpenPlayback(ma_context* ctx, ma_device* device, const std::string& name, ma_device_data_proc callback, void* user) {
    ma_device_id id;
    bool named = !name.empty() && findDevice(ctx, false, name, &id);
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.playback.pDeviceID = named ? &id : nullptr;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = callback;
    cfg.pUserData = user;
    if (ma_device_init(ctx, &cfg, device) != MA_SUCCESS) return false;
    if (ma_device_start(device) != MA_SUCCESS) {
        ma_device_uninit(device);
        return false;
    }
    return true;
}

struct Microphone::Impl {
    std::function<void(const float*, size_t)> frame;
    std::function<void()> wake;
    mutable std::mutex mu;
    ma_context ctx{};
    bool ctxUp = false;
    ma_device device{};
    bool running = false;
    std::string error;
    std::vector<float> pending;
    std::atomic<float> level{-100};
    float shown = -100;  // the level last woken for

    static void callback(ma_device* d, void*, const void* in, ma_uint32 frames) {
        Impl* self = static_cast<Impl*>(d->pUserData);
        const float* s = static_cast<const float*>(in);
        self->pending.insert(self->pending.end(), s, s + frames);
        size_t at = 0;
        while (self->pending.size() - at >= kFrameSamples) {
            const float* f = self->pending.data() + at;
            float db = LevelDb(f, kFrameSamples);
            self->level = db;
            self->frame(f, kFrameSamples);
            at += kFrameSamples;
            if (std::abs(db - self->shown) > 3) {
                self->shown = db;
                self->wake();
            }
        }
        self->pending.erase(self->pending.begin(), self->pending.begin() + at);
    }
};

Microphone::Microphone(std::function<void(const float*, size_t)> frame, std::function<void()> wake)
    : impl_(std::make_unique<Impl>()) {
    impl_->frame = std::move(frame);
    impl_->wake = std::move(wake);
}

Microphone::~Microphone() {
    Stop();
    if (impl_->ctxUp) ma_context_uninit(&impl_->ctx);
}

bool Microphone::Start(const std::string& device) {
    Stop();
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->ctxUp) {
        if (ma_context_init(nullptr, 0, nullptr, &impl_->ctx) != MA_SUCCESS) {
            impl_->error = "Windows audio isn't available.";
            return false;
        }
        impl_->ctxUp = true;
    }
    ma_device_id id;
    bool named = !device.empty() && findDevice(&impl_->ctx, true, device, &id);
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.capture.pDeviceID = named ? &id : nullptr;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = Impl::callback;
    cfg.pUserData = impl_.get();
    impl_->pending.clear();
    if (ma_device_init(&impl_->ctx, &cfg, &impl_->device) != MA_SUCCESS) {
        impl_->error = "Couldn't open the microphone.";
        return false;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->error = "The microphone wouldn't start.";
        return false;
    }
    impl_->running = true;
    impl_->error.clear();
    return true;
}

void Microphone::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) return;
    ma_device_uninit(&impl_->device);
    impl_->running = false;
    impl_->level = -100;
}

bool Microphone::Running() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->running;
}

std::string Microphone::Error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

float Microphone::Level() const { return impl_->level; }

}  // namespace disky
