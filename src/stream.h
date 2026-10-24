// Showing a Go Live stream: Windows' H.264 decoder (a Media Foundation
// transform) on its own thread, each picture turned into BGRA for a texture
// ImGui draws.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace disky {

class StreamDecoder {
public:
    // wake is called from the decoding thread when there's a new picture.
    StreamDecoder(ID3D11Device* device, std::function<void()> wake);
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    // Push queues a frame (Annex B) from any thread.
    void Push(std::string frame);
    // Frame uploads the newest picture, if there's a new one, and returns
    // the texture; nullptr before the first. UI thread only.
    ID3D11ShaderResourceView* Frame();
    int Width() const { return width_; }
    int Height() const { return height_; }
    // Failed says the decoder couldn't take what it was given, so a key
    // frame should be asked for; reading it clears it.
    bool TakeFailed() { return failed_.exchange(false); }
    std::string Error() const;
    // Decoded counts pictures, for frames per second.
    int Decoded() const { return decoded_; }
    // Info is how the first picture was laid out, for the log.
    std::string Info() {
        std::lock_guard<std::mutex> lock(picMu_);
        return info_;
    }

private:
    void run();

    ID3D11Device* device_;
    std::function<void()> wake_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool stop_ = false;

    // The newest picture, waiting for the UI thread.
    std::mutex picMu_;
    std::vector<uint8_t> picture_;
    int picW_ = 0, picH_ = 0;
    bool fresh_ = false;
    std::string error_;
    std::string info_;  // the first picture's layout, for the log

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    int texW_ = 0, texH_ = 0;
    std::atomic<int> width_{0}, height_{0}, decoded_{0};
    std::atomic<bool> failed_{false};
};

}  // namespace disky
