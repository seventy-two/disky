#include "video.h"

#include <mfapi.h>

#include "net.h"

using Microsoft::WRL::ComPtr;

namespace disky {

namespace {

// Callback passes the media engine's events to the player.
class Callback : public IMFMediaEngineNotify {
public:
    explicit Callback(VideoPlayer* player) : player_(player) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == __uuidof(IUnknown)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        if (VideoPlayer* p = player_) p->Notify(event, param1, param2);
        return S_OK;
    }
    void Detach() { player_ = nullptr; }

private:
    std::atomic<VideoPlayer*> player_;
    LONG refs_ = 1;
};

// The largest a video's texture gets; bigger ones are scaled down.
constexpr int kMaxWidth = 1280;

}  // namespace

VideoPlayer::VideoPlayer(ID3D11Device* device, std::function<void()> wake) : device_(device), wake_(std::move(wake)) {
    MFStartup(MF_VERSION);
}

VideoPlayer::~VideoPlayer() {
    Stop();
    manager_.Reset();
    MFShutdown();
}

void VideoPlayer::Notify(DWORD event, DWORD_PTR param1, DWORD param2) {
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_ERROR:
        failed_ = true;
        errorCode_ = DWORD(param1);
        break;
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
        if (engine_) {
            DWORD w = 0, h = 0;
            engine_->GetNativeVideoSize(&w, &h);
            width_ = int(w);
            height_ = int(h);
        }
        break;
    }
    if (wake_) wake_();
}

void VideoPlayer::Play(const std::string& url, float volume, bool loop) {
    Stop();
    url_ = url;
    loop_ = loop;
    failed_ = false;
    errorCode_ = 0;
    if (!manager_) {
        if (FAILED(MFCreateDXGIDeviceManager(&resetToken_, &manager_)) ||
            FAILED(manager_->ResetDevice(device_, resetToken_))) {
            manager_.Reset();
            failed_ = true;
            return;
        }
    }
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(MFCreateAttributes(&attrs, 3))) {
        failed_ = true;
        return;
    }
    notify_.Attach(new Callback(this));
    attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager_.Get());
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get());
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (FAILED(factory->CreateInstance(0, attrs.Get(), &engine_))) {
        failed_ = true;
        return;
    }
    BSTR src = SysAllocString(Widen(url).c_str());
    engine_->SetSource(src);
    SysFreeString(src);
    engine_->SetVolume(loop ? 0 : volume);
    engine_->SetMuted(loop);
    engine_->SetLoop(loop);
    engine_->Play();
    started_ = true;
}

void VideoPlayer::Stop() {
    // Detach first, so no event arrives while the engine goes.
    if (notify_) {
        static_cast<Callback*>(notify_.Get())->Detach();
        notify_.Reset();
    }
    if (engine_) {
        engine_->Shutdown();
        engine_.Reset();
    }
    srv_.Reset();
    texture_.Reset();
    url_.clear();
    width_ = height_ = 0;
    haveFrame_ = false;
    started_ = false;
}

ID3D11ShaderResourceView* VideoPlayer::Frame() {
    if (!engine_ || failed_) return haveFrame_ ? srv_.Get() : nullptr;
    int w = width_, h = height_;
    if (w <= 0 || h <= 0) return nullptr;
    if (w > kMaxWidth) {
        h = h * kMaxWidth / w;
        w = kMaxWidth;
    }
    D3D11_TEXTURE2D_DESC have{};
    if (texture_) texture_->GetDesc(&have);
    if (!texture_ || int(have.Width) != w || int(have.Height) != h) {
        srv_.Reset();
        texture_.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w);
        d.Height = UINT(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &texture_)) ||
            FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) {
            texture_.Reset();
            return nullptr;
        }
        haveFrame_ = false;
    }
    LONGLONG pts;
    if (engine_->OnVideoStreamTick(&pts) == S_OK) {
        MFVideoNormalizedRect src{0, 0, 1, 1};
        RECT dst{0, 0, w, h};
        MFARGB black{0, 0, 0, 255};
        if (SUCCEEDED(engine_->TransferVideoFrame(texture_.Get(), &src, &dst, &black))) haveFrame_ = true;
    }
    return haveFrame_ ? srv_.Get() : nullptr;
}

double VideoPlayer::Position() const { return engine_ ? engine_->GetCurrentTime() : 0; }

double VideoPlayer::Duration() const {
    if (!engine_) return 0;
    double d = engine_->GetDuration();
    return d == d && d < 1e9 ? d : 0;  // NaN or infinite while unknown
}

bool VideoPlayer::Paused() const { return engine_ && engine_->IsPaused(); }
bool VideoPlayer::Ended() const { return engine_ && engine_->IsEnded(); }

void VideoPlayer::TogglePause() {
    if (!engine_) return;
    if (engine_->IsEnded()) {
        engine_->SetCurrentTime(0);
        engine_->Play();
    } else if (engine_->IsPaused()) {
        engine_->Play();
    } else {
        engine_->Pause();
    }
}

void VideoPlayer::Seek(double seconds) {
    if (engine_) engine_->SetCurrentTime(seconds);
}

void VideoPlayer::SetVolume(float v) {
    if (engine_ && !loop_) engine_->SetVolume(v);
}

std::string VideoPlayer::Error() const {
    if (!failed_) return {};
    switch (errorCode_) {
    case MF_MEDIA_ENGINE_ERR_NETWORK: return "couldn't download the video";
    case MF_MEDIA_ENGINE_ERR_DECODE: return "couldn't decode the video";
    case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: return "Windows can't play this format";
    }
    return "couldn't play the video";
}

}  // namespace disky
