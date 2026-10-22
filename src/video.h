// Inline video: one at a time, played by Media Foundation's media engine
// straight from its URL, each frame copied into a texture ImGui draws.
#pragma once

#include <d3d11.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <string>

namespace disky {

class VideoPlayer {
public:
    // The device must be made with video support and multithread
    // protection on; wake is called from Media Foundation's threads.
    VideoPlayer(ID3D11Device* device, std::function<void()> wake);
    ~VideoPlayer();

    // Play starts a video, stopping any other. A loop (a GIF) repeats,
    // silent.
    void Play(const std::string& url, float volume, bool loop = false);
    bool Looping() const { return loop_; }
    void Stop();
    const std::string& Url() const { return url_; }
    bool Active() const { return engine_ != nullptr; }

    // Frame copies the newest frame into the texture and returns it, or
    // nullptr before the first. Call it on the UI thread each frame.
    ID3D11ShaderResourceView* Frame();
    int Width() const { return width_; }
    int Height() const { return height_; }
    double Position() const;
    double Duration() const;
    bool Paused() const;
    bool Ended() const;
    void TogglePause();
    void Seek(double seconds);
    void SetVolume(float v);
    // Error is why the video can't play, e.g. a format Windows can't decode.
    std::string Error() const;

    // Notify is called by the engine's callback.
    void Notify(DWORD event, DWORD_PTR param1, DWORD param2);

private:
    ID3D11Device* device_;
    std::function<void()> wake_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    UINT resetToken_ = 0;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<IMFMediaEngineNotify> notify_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    std::string url_;
    std::atomic<int> width_{0}, height_{0};
    std::atomic<bool> failed_{false}, haveFrame_{false};
    std::atomic<DWORD> errorCode_{0};
    bool started_ = false;
    bool loop_ = false;
};

}  // namespace disky
