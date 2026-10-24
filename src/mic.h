// The microphone, for voice: capturing it in 20ms frames, and the audio
// devices there are to pick from.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace disky {

// AudioDevices lists the names of the inputs (capture) or outputs.
std::vector<std::string> AudioDevices(bool capture);

class Microphone {
public:
    // frame is called on the capture thread with each 20ms of mono audio
    // (960 samples at 48kHz). wake is called when the level's changed
    // enough to redraw a meter.
    Microphone(std::function<void(const float*, size_t)> frame, std::function<void()> wake);
    ~Microphone();
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;

    // Start opens the named input, or Windows' default if it's empty or
    // gone; false (with Error) if there's none to open.
    bool Start(const std::string& device);
    void Stop();
    bool Running() const;
    std::string Error() const;
    // Level is the last frame's loudness in dBFS.
    float Level() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace disky
