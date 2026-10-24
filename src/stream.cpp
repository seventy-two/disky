#include "stream.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <cstring>

namespace disky {

using Microsoft::WRL::ComPtr;

namespace {

// Windows' H.264 decoder (CLSID_MSH264DecoderMFT from wmcodecdsp.h).
const CLSID kH264Decoder = {0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};

// nv12ToBgra converts a picture, BT.709 or BT.601, limited range.
void nv12ToBgra(const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, bool bt709, uint8_t* out) {
    // Coefficients in 16.16 fixed point.
    const int cy = 76309;  // 255/219
    const int crv = bt709 ? 117489 : 104597, cgu = bt709 ? 13975 : 25675, cgv = bt709 ? 34925 : 53279,
              cbu = bt709 ? 138438 : 132201;
    auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int row = 0; row < h; row++) {
        const uint8_t* yr = y + size_t(row) * pitch;
        const uint8_t* uvr = uv + size_t(row / 2) * pitch;
        uint8_t* o = out + size_t(row) * w * 4;
        for (int x = 0; x < w; x++) {
            int Y = (yr[x] - 16) * cy;
            int U = uvr[x & ~1] - 128, V = uvr[(x & ~1) + 1] - 128;
            o[0] = clamp((Y + cbu * U) >> 16);
            o[1] = clamp((Y - cgu * U - cgv * V) >> 16);
            o[2] = clamp((Y + crv * V) >> 16);
            o[3] = 255;
            o += 4;
        }
    }
}

}  // namespace

StreamDecoder::StreamDecoder(ID3D11Device* device, std::function<void()> wake) : device_(device), wake_(std::move(wake)) {
    thread_ = std::thread(&StreamDecoder::run, this);
}

StreamDecoder::~StreamDecoder() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void StreamDecoder::Push(std::string frame) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(frame));
        // Falling behind: better to skip than lag (the decoder conceals).
        while (queue_.size() > 30) queue_.pop_front();
    }
    cv_.notify_one();
}

std::string StreamDecoder::Error() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(picMu_));
    return error_;
}

ID3D11ShaderResourceView* StreamDecoder::Frame() {
    std::lock_guard<std::mutex> lock(picMu_);
    if (fresh_ && picW_ > 0) {
        if (!texture_ || texW_ != picW_ || texH_ != picH_) {
            texture_.Reset();
            srv_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = UINT(picW_);
            d.Height = UINT(picH_);
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &texture_)) ||
                FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) {
                texture_.Reset();
                srv_.Reset();
                return nullptr;
            }
            texW_ = picW_;
            texH_ = picH_;
        }
        ComPtr<ID3D11DeviceContext> ctx;
        device_->GetImmediateContext(&ctx);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int row = 0; row < picH_; row++)
                memcpy(static_cast<uint8_t*>(m.pData) + size_t(row) * m.RowPitch, picture_.data() + size_t(row) * picW_ * 4,
                       size_t(picW_) * 4);
            ctx->Unmap(texture_.Get(), 0);
        }
        fresh_ = false;
    }
    return srv_.Get();
}

void StreamDecoder::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(picMu_);
        error_ = why;
        wake_();
    };
    ComPtr<IMFTransform> mft;
    if (FAILED(CoCreateInstance(kH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)))) {
        fail("Windows' H.264 decoder isn't available.");
    } else {
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(mft->SetInputType(0, in.Get(), 0))) {
            fail("The H.264 decoder wouldn't start.");
            mft.Reset();
        }
    }

    int w = 0, h = 0, pitchH = 0, stride = 0;
    bool bt709 = true;
    // setOutput picks NV12 and reads the picture's size.
    auto setOutput = [&]() -> bool {
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub;
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                if (FAILED(mft->SetOutputType(0, t.Get(), 0))) return false;
                UINT32 fw = 0, fh = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &fw, &fh);
                pitchH = int(fh);
                w = int(fw);
                h = int(fh);
                // Rows are often padded wider than the picture.
                UINT32 st = 0;
                stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? int(INT32(st)) : 0;
                MFVideoArea area;
                if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
                    w = int(area.Area.cx);
                    h = int(area.Area.cy);
                }
                UINT32 matrix = 0;
                bt709 = FAILED(t->GetUINT32(MF_MT_YUV_MATRIX, &matrix)) ? h >= 720 : matrix == MFVideoTransferMatrix_BT709;
                return true;
            }
        }
    };
    if (mft) {
        setOutput();
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    LONGLONG time = 0;
    std::vector<uint8_t> bgra;
    for (;;) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            frame = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!mft) continue;
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateMemoryBuffer(DWORD(frame.size()), &buf)) || FAILED(MFCreateSample(&sample))) continue;
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, frame.data(), frame.size());
        buf->Unlock();
        buf->SetCurrentLength(DWORD(frame.size()));
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time);
        sample->SetSampleDuration(166666);
        time += 166666;
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // Drain first, then try again below.
        } else if (FAILED(hr)) {
            failed_ = true;
            continue;
        }
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);
            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> outSample;
            ComPtr<IMFMediaBuffer> outBuf;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                MFCreateSample(&outSample);
                MFCreateMemoryBuffer(info.cbSize, &outBuf);
                outSample->AddBuffer(outBuf.Get());
                out.pSample = outSample.Get();
            }
            DWORD status = 0;
            HRESULT r = mft->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (r == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (r == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!setOutput()) {
                    fail("The stream's picture format isn't one disky can show.");
                    break;
                }
                continue;
            }
            if (FAILED(r)) {
                failed_ = true;
                break;
            }
            IMFSample* got = out.pSample;
            ComPtr<IMFMediaBuffer> mb;
            // The sample's own buffer, as a 2D buffer, knows its pitch; a
            // copied contiguous one doesn't.
            if (got && SUCCEEDED(got->GetBufferByIndex(0, &mb)) && w > 0 && h > 0) {
                ComPtr<IMF2DBuffer> b2;
                BYTE* data = nullptr;
                LONG pitch = 0;
                DWORD len = 0;
                bool locked2d = SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch));
                if (!locked2d) {
                    data = nullptr;
                    mb->Lock(&data, nullptr, &len);
                    // The stride from the type, else worked out from the size
                    // (the luma plane plus half again for chroma).
                    pitch = stride > 0 ? stride : pitchH > 0 ? LONG(len * 2 / (3 * DWORD(pitchH))) : w;
                }
                // The chroma plane follows the (padded) luma plane.
                int planeH = pitchH;
                if (!locked2d && pitch > 0 && len >= DWORD(pitch) * 3 / 2) planeH = int(len * 2 / 3 / DWORD(pitch));
                if (data && pitch >= w) {
                    bgra.resize(size_t(w) * h * 4);
                    nv12ToBgra(data, data + size_t(pitch) * planeH, int(pitch), w, h, bt709, bgra.data());
                    {
                        std::lock_guard<std::mutex> lock(picMu_);
                        picture_.swap(bgra);
                        picW_ = w;
                        picH_ = h;
                        fresh_ = true;
                        error_.clear();
                    }
                    width_ = w;
                    height_ = h;
                    decoded_++;
                    if (decoded_ == 1) {
                        std::lock_guard<std::mutex> lock(picMu_);
                        info_ = std::to_string(w) + "x" + std::to_string(h) + " pitch " + std::to_string(pitch) + " plane " +
                                std::to_string(planeH) + (locked2d ? " 2D" : " flat");
                    }
                    wake_();
                }
                if (locked2d)
                    b2->Unlock2D();
                else if (data)
                    mb->Unlock();
            }
            if (out.pSample && !outSample) out.pSample->Release();  // the decoder's own
        }
        if (hr == MF_E_NOTACCEPTING) mft->ProcessInput(0, sample.Get(), 0);
    }
    mft.Reset();
    MFShutdown();
    CoUninitialize();
}

}  // namespace disky
