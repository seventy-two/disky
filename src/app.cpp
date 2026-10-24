// The client's state and what it does: signing in, the gateway's events,
// loading, sending and acknowledging messages. Drawing is in ui.cpp.
#include "app.h"

#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace disky {

static App* hookApp = nullptr;  // the push-to-talk hooks are global, so they find the app here

namespace {

// The web app's build number when this was written, until a newer one
// has been read from discord.com.
constexpr int kKnownBuild = 646578;

int64_t unixDay() {
    using namespace std::chrono;
    return duration_cast<hours>(system_clock::now().time_since_epoch()).count() / 24;
}

std::string mimeFor(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = wchar_t(towlower(c));
    if (ext == L"png") return "image/png";
    if (ext == L"jpg" || ext == L"jpeg") return "image/jpeg";
    if (ext == L"gif") return "image/gif";
    if (ext == L"webp") return "image/webp";
    if (ext == L"mp4") return "video/mp4";
    if (ext == L"webm") return "video/webm";
    if (ext == L"mov") return "video/quicktime";
    if (ext == L"txt" || ext == L"log") return "text/plain";
    return "application/octet-stream";
}

// dibToPng turns a clipboard bitmap into PNG bytes.
std::string dibToPng(const void* dib, size_t size) {
    // Make it a .bmp file in memory and let WIC read that.
    const auto* info = static_cast<const BITMAPINFOHEADER*>(dib);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = DWORD(sizeof(fh) + size);
    DWORD colours = info->biClrUsed ? info->biClrUsed : (info->biBitCount <= 8 ? 1u << info->biBitCount : 0);
    DWORD masks = (info->biCompression == BI_BITFIELDS && info->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
    fh.bfOffBits = DWORD(sizeof(fh) + info->biSize + masks + colours * 4);
    std::string bmp((const char*)&fh, sizeof(fh));
    bmp.append((const char*)dib, size);

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> in, out;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> outFrame;
    ComPtr<IStream> mem;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&in)) || FAILED(in->InitializeFromMemory((BYTE*)bmp.data(), DWORD(bmp.size()))) ||
        FAILED(wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return {};
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&outFrame, nullptr)) ||
        FAILED(outFrame->Initialize(nullptr)))
        return {};
    UINT w, h;
    frame->GetSize(&w, &h);
    outFrame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    outFrame->SetPixelFormat(&fmt);
    ComPtr<IWICFormatConverter> conv;
    // Clipboard bitmaps usually have no real alpha: treat them as opaque.
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(outFrame->WriteSource(conv.Get(), nullptr)) || FAILED(outFrame->Commit()) || FAILED(enc->Commit()))
        return {};
    HGLOBAL g;
    GetHGlobalFromStream(mem.Get(), &g);
    STATSTG st;
    mem->Stat(&st, STATFLAG_NONAME);
    std::string png((const char*)GlobalLock(g), size_t(st.cbSize.QuadPart));
    GlobalUnlock(g);
    return png;
}

}  // namespace

void OpenURL(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return;
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

App::App(HWND hwnd, ID3D11Device* device, std::function<void()> wake)
    : hwnd_(hwnd), device_(device), wake_(std::move(wake)) {
    config_ = LoadConfig();
    fonts_ = LoadFonts();
    images_ = std::make_unique<ImageCache>(device_, wake_);
    video_ = std::make_unique<VideoPlayer>(device_, wake_);
    gif_ = std::make_unique<VideoPlayer>(device_, wake_);
    sender_ = std::make_unique<Worker>();
    fetcher_ = std::make_unique<Worker>();
    gateway_ = std::make_unique<Gateway>(wake_);
    startChoice_ = StartsWithWindows();
    identity_ = MakeIdentity(config_.buildNumber ? config_.buildNumber : kKnownBuild);

    // Read the web app's current build number once a day, for next time
    // (or now, if there's no account yet).
    if (config_.buildDay != unixDay()) {
        std::string ua = identity_.userAgent;
        fetcher_->Run([this, ua] {
            Http http(ua);
            int build = FetchBuildNumber(http);
            post([this, build] {
                if (build <= 0) return;
                config_.buildNumber = build;
                config_.buildDay = unixDay();
                SaveConfig(config_);
                if (!api_) identity_ = MakeIdentity(build);
            });
        });
    }
    if (!config_.token.empty()) {
        signIn(config_.token);
    } else {
        remote_ = std::make_unique<RemoteAuth>(identity_, wake_);
        remote_->Start();
    }
}

App::~App() {
    leaveVoice();
    if (mic_) mic_->Stop();
    if (keyHook_) UnhookWindowsHookEx(keyHook_);
    if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
    hookApp = nullptr;
    SaveConfig(config_);
    if (gateway_) gateway_->Stop();
    if (remote_) remote_->Stop();
    // Workers finish their jobs first; their posts then go nowhere.
    sender_.reset();
    fetcher_.reset();
}

void App::post(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lock(postMu_);
        posted_.push_back(std::move(f));
    }
    wake_();
}

void App::SetScale(float scale) {
    scale_ = scale;
    ApplyTheme(dark_, scale_);
    layouts_.clear();
}

void App::SetDark(bool dark) {
    dark_ = dark;
    ApplyTheme(dark_, scale_);
}

bool App::BeforeFrame() {
    if (!LoadNeededFonts(fonts_)) return false;
    // Text laid out before measured the missing glyphs: lay it out again.
    layouts_.clear();
    textLayouts_.clear();
    return true;
}

void App::SaveWindow() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    RECT r = wp.rcNormalPosition;
    config_.x = r.left;
    config_.y = r.top;
    config_.w = r.right - r.left;
    config_.h = r.bottom - r.top;
    config_.maximised = wp.showCmd == SW_SHOWMAXIMIZED;
    SaveConfig(config_);
}

// --- signing in ---

void App::signIn(const std::string& token) {
    generation_++;
    config_.token = token;
    SaveConfig(config_);
    loginError_.clear();
    api_ = std::make_unique<Api>(identity_, token);
    ready_ = false;
    status_ = "Connecting…";
    gateway_->Start(identity_, token);
}

void App::signOut() {
    generation_++;
    dropVoice();
    gateway_->Stop();
    video_->Stop();
    gif_->Stop();
    api_.reset();
    config_.token.clear();
    SaveConfig(config_);
    store_ = Store();
    ready_ = false;
    guild_ = channel_ = 0;
    layouts_.clear();
    listFor_.clear();
    listRanges_.clear();
    drafts_.clear();
    commandIndex_.clear();
    commandsLoading_.clear();
    remote_ = std::make_unique<RemoteAuth>(identity_, wake_);
    remote_->Start();
}

void App::joinVoice(uint64_t guild, uint64_t channel) {
    if (voiceChannel_ == channel && voice_ && voice_->GetState() != VoiceConnection::State::Failed) return;
    if (voice_) dropVoice();
    voiceGuild_ = guild;
    voiceChannel_ = channel;
    voiceSession_.clear();
    voiceToken_.clear();
    voiceEndpoint_.clear();
    sendVoiceState();
}

void App::leaveVoice() {
    if (!voiceChannel_) return;
    gateway_->Send(4, {{"guild_id", IdString(voiceGuild_)},
                       {"channel_id", nullptr},
                       {"self_mute", config_.micMuted || config_.deafened},
                       {"self_deaf", config_.deafened}});
    dropVoice();
}

void App::sendVoiceState() {
    // Deafened mutes too, as Discord does.
    gateway_->Send(4, {{"guild_id", IdString(voiceGuild_)},
                       {"channel_id", IdString(voiceChannel_)},
                       {"self_mute", config_.micMuted || config_.deafened},
                       {"self_deaf", config_.deafened},
                       {"self_video", false}});
}

void App::connectVoice() {
    if (voice_ || !voiceChannel_ || voiceSession_.empty() || voiceToken_.empty() || voiceEndpoint_.empty()) return;
    VoiceConnection::Params p;
    p.userId = store_.me;
    p.guildId = voiceGuild_;
    p.channelId = voiceChannel_;
    p.sessionId = voiceSession_;
    p.token = voiceToken_;
    p.endpoint = voiceEndpoint_;
    p.userAgent = identity_.userAgent;
    p.output = config_.output;
    voice_ = std::make_unique<VoiceConnection>(p, wake_);
    voice_->SetDeafened(config_.deafened);
    applySending();
    {
        std::lock_guard<std::mutex> lock(micMu_);
        micSink_ = voice_.get();
    }
    for (const auto& [id, v] : config_.voiceVolumes) voice_->SetVolume(std::strtoull(id.c_str(), nullptr, 10), v);
}

void App::dropVoice() {
    {
        std::lock_guard<std::mutex> lock(micMu_);
        micSink_ = nullptr;
    }
    voiceChannel_ = voiceGuild_ = 0;
    voiceSession_.clear();
    voiceToken_.clear();
    voiceEndpoint_.clear();
    // Closing waits on its threads, so not on this one.
    if (voice_) {
        std::shared_ptr<VoiceConnection> old(voice_.release());
        fetcher_->Run([old]() mutable { old.reset(); });
    }
}

void App::applySending() {
    if (!voice_) return;
    auto mode = config_.micMuted || config_.deafened ? VoiceConnection::Send::Off
                : config_.pushToTalk               ? VoiceConnection::Send::PushToTalk
                                                    : VoiceConnection::Send::VoiceActivity;
    voice_->SetSending(mode, config_.threshold);
}

LRESULT CALLBACK App::keyHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && hookApp) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN, up = w == WM_KEYUP || w == WM_SYSKEYUP;
        if ((down || up) && hookApp->pushToTalk(int(k->vkCode), down)) return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK App::mouseHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && hookApp) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        int vk = 0;
        if (w == WM_MBUTTONDOWN || w == WM_MBUTTONUP) vk = VK_MBUTTON;
        if (w == WM_XBUTTONDOWN || w == WM_XBUTTONUP) vk = HIWORD(m->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (vk && hookApp->pushToTalk(vk, w == WM_MBUTTONDOWN || w == WM_XBUTTONDOWN)) return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

bool App::pushToTalk(int vk, bool down) {
    if (bindingKey_) {
        if (!down) return false;
        bindingKey_ = false;
        if (vk != VK_ESCAPE) {
            config_.pttKey = vk;
            SaveConfig(config_);
        }
        wake_();
        return true;
    }
    if (vk != config_.pttKey || down == pttHeld_) return false;
    pttHeld_ = down;
    if (voice_) voice_->SetPushToTalk(down);
    wake_();
    return false;
}

void App::updateVoice() {
    bool inCall = voice_ && voice_->GetState() == VoiceConnection::State::Connected;
    bool want = (inCall && !config_.micMuted && !config_.deafened) || settingsVisible_;
    if (!mic_)
        mic_ = std::make_unique<Microphone>(
            [this](const float* f, size_t n) {
                std::lock_guard<std::mutex> lock(micMu_);
                if (micSink_) micSink_->PushMic(f, n);
            },
            wake_);
    if (want && (!mic_->Running() || micDevice_ != config_.input)) {
        // A mic that won't open is tried again now and then, not every frame.
        double t = GetTickCount64() / 1000.0;
        if (micDevice_ != config_.input || t - micFailedAt_ > 5) {
            micDevice_ = config_.input;
            if (!mic_->Start(config_.input)) micFailedAt_ = t;
        }
    } else if (!want && mic_->Running()) {
        mic_->Stop();
    }
    // The push-to-talk hooks, only while they could be needed.
    bool hook = bindingKey_ || (voiceChannel_ && config_.pushToTalk && config_.pttKey);
    if (hook && !keyHook_) {
        hookApp = this;
        keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyHook, GetModuleHandleW(nullptr), 0);
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    } else if (!hook && keyHook_) {
        UnhookWindowsHookEx(keyHook_);
        if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
        keyHook_ = mouseHook_ = nullptr;
        pttHeld_ = false;
    }
}

void App::Pump() {
    std::vector<std::function<void()>> posted;
    {
        std::lock_guard<std::mutex> lock(postMu_);
        posted.swap(posted_);
    }
    for (auto& f : posted) f();
    images_->Upload();

    if (remote_) {
        RemoteAuth::Status s = remote_->Get();
        if (s.stage == RemoteAuth::Status::Stage::Done && !s.token.empty()) {
            remote_->Stop();
            remote_.reset();
            signIn(s.token);
        }
    }

    std::vector<Dispatch> events;
    gateway_->Drain(events);
    for (const auto& d : events) onDispatch(d);
    if (ready_) updateVoice();

    // Read what's on screen: the open channel, while the window has focus
    // and the newest messages are in view.
    if (ready_ && channel_ && atBottom_ && GetForegroundWindow() == hwnd_ && !IsIconic(hwnd_) &&
        (store_.Unread(channel_) || store_.Mentions(channel_) > 0)) {
        unsigned long long t = GetTickCount64();
        if (t - acked_[channel_] > 1500) ack(channel_);
    }
}

namespace {

// shape describes JSON's structure without its values (set DISKY_SHAPE to
// write READY's to %APPDATA%\\disky\\ready-shape.txt): keys and types,
// the first element of arrays. For debugging Discord's payloads without
// writing anyone's data down.
void shape(const json& j, std::string& out, int depth, const std::string& indent) {
    if (depth > 6) return;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            out += indent + it.key() + ": " + it->type_name();
            if (it->is_array()) out += "[" + std::to_string(it->size()) + "]";
            out += "\n";
            shape(*it, out, depth + 1, indent + "  ");
        }
    } else if (j.is_array() && !j.empty()) {
        out += indent + "[0]: " + std::string(j[0].type_name()) + "\n";
        shape(j[0], out, depth + 1, indent + "  ");
    }
}

}  // namespace

void App::onDispatch(const Dispatch& d) {
    if ((d.type == "READY" || d.type == "READY_SUPPLEMENTAL") && GetEnvironmentVariableW(L"DISKY_SHAPE", nullptr, 0)) {
        std::string out;
        shape(d.data, out, 0, "");
        wchar_t path[MAX_PATH];
        ExpandEnvironmentStringsW(d.type == "READY" ? L"%APPDATA%\\disky\\ready-shape.txt"
                                                    : L"%APPDATA%\\disky\\ready-supplemental-shape.txt",
                                  path, MAX_PATH);
        std::ofstream(path, std::ios::binary) << out;
    }
    if (d.type.empty()) {
        if (d.data.value("auth_failed", false)) {
            loginError_ = "Discord signed this session out. Sign in again.";
            signOut();
        } else if (d.data.contains("disconnected")) {
            status_ = "Reconnecting…";
        }
        return;
    }
    if (d.type == "GUILD_MEMBER_LIST_UPDATE") {
        // The first list sent for the guild after subscribing is the open
        // channel's.
        uint64_t gid = ParseId(d.data["guild_id"]);
        bool sync = false;
        for (const auto& op : d.data.value("ops", json::array()))
            if (op.value("op", "") == "SYNC") sync = true;
        if (gid == guild_ && subscribedChannel_ && (sync || !listFor_.count(subscribedChannel_)))
            listFor_[subscribedChannel_] = d.data.value("id", "");
    }
    if (d.type == "TYPING_START") {
        uint64_t c = ParseId(d.data["channel_id"]), u = ParseId(d.data["user_id"]);
        if (auto mem = d.data.find("member"); mem != d.data.end() && mem->is_object() && d.data.contains("guild_id")) {
            json m = *mem;
            m["guild_id"] = d.data["guild_id"];
            store_.Apply("GUILD_MEMBER_UPDATE", m);
        }
        if (u != store_.me) typing_[c][u] = GetTickCount64() + 10000;
        return;
    }
    if (d.type == "MESSAGE_CREATE")
        if (auto t = typing_.find(ParseId(d.data["channel_id"])); t != typing_.end())
            t->second.erase(ParseId(d.data["author"]["id"]));
    if (d.type == "APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE") {
        if (d.data.value("nonce", "") == acNonce_) {
            acChoices_.clear();
            for (const auto& ch : d.data.value("choices", json::array()))
                acChoices_.push_back({ch.value("name", ""), ch.value("value", json())});
            acFor_ = acPendingFor_;
        }
        return;
    }
    if (d.type == "INTERACTION_SUCCESS" || d.type == "INTERACTION_FAILURE") {
        auto it = commandNonces_.find(d.data.value("nonce", ""));
        if (it == commandNonces_.end()) return;
        if (d.type == "INTERACTION_FAILURE") composeError_ = it->second + " didn't work: the app didn't respond.";
        commandNonces_.erase(it);
        if (commandNonces_.empty()) commandStatus_.clear();
        return;
    }
    if (d.type == "VOICE_STATE_UPDATE" && ParseId(d.data.value("user_id", json())) == store_.me && voiceChannel_) {
        uint64_t ch = ParseId(d.data.value("channel_id", json()));
        if (d.data.value("session_id", "") != sessionId_ || !ch) {
            // Left, removed, or joined from another device, which takes over.
            dropVoice();
        } else if (ch != voiceChannel_) {
            // Moved: same server, new channel, so a new call.
            voiceChannel_ = ch;
            voiceGuild_ = ParseId(d.data.value("guild_id", json()));
            {
                std::lock_guard<std::mutex> lock(micMu_);
                micSink_ = nullptr;
            }
            voice_.reset();
            connectVoice();
        } else {
            voiceSession_ = d.data.value("session_id", "");
            connectVoice();
        }
    }
    if (d.type == "VOICE_SERVER_UPDATE" && voiceChannel_ && ParseId(d.data.value("guild_id", json())) == voiceGuild_) {
        // A null endpoint means the server's moving; another update follows.
        voiceToken_ = d.data.value("token", "");
        voiceEndpoint_ = d.data["endpoint"].is_string() ? d.data["endpoint"].get<std::string>() : "";
        if (voice_) {
            {
                std::lock_guard<std::mutex> lock(micMu_);
                micSink_ = nullptr;
            }
            std::shared_ptr<VoiceConnection> old(voice_.release());
            fetcher_->Run([old]() mutable { old.reset(); });
        }
        connectVoice();
    }
    auto effects = store_.Apply(d.type, d.data);
    if (d.type == "READY") {
        // A new session ends any call the old one had: join again.
        if (voiceChannel_) {
            uint64_t g = voiceGuild_, c = voiceChannel_;
            dropVoice();
            joinVoice(g, c);
        }
        ready_ = true;
        sessionId_ = d.data.value("session_id", "");
        commandIndex_.clear();
        status_.clear();
        listFor_.clear();
        listRanges_.clear();
        layouts_.clear();
        subscribedChannel_ = 0;
        uint64_t g = config_.guild, c = config_.channel;
        if (g && !store_.guilds.count(g)) g = c = 0;
        if (c && !store_.channels.count(c)) c = 0;
        open(g, c);
    } else if (d.type == "RESUMED") {
        status_.clear();
    }
    for (const auto& fx : effects) {
        switch (fx.type) {
        case Effect::Type::Mention: notifyMention(store_.ParseMessage(d.data)); break;
        case Effect::Type::ChannelGone:
            if (fx.channelId == channel_) open(guild_, 0);
            break;
        case Effect::Type::GuildGone:
            if (fx.guildId == guild_) open(0, 0);
            break;
        default: break;
        }
    }
    if (d.type == "MESSAGE_CREATE" || d.type == "MESSAGE_UPDATE") {
        uint64_t c = ParseId(d.data["channel_id"]);
        if (c == channel_) askMembers(c);
    }
}

void App::notifyMention(const Message& m) {
    if (!config_.notify) return;
    bool looking = m.channelId == channel_ && GetForegroundWindow() == hwnd_ && !IsIconic(hwnd_) && atBottom_;
    if (looking) return;
    Resolver names = resolver(m.guildId);
    std::string author = store_.DisplayName(m.guildId, m.authorId);
    std::string title;
    auto c = store_.channels.find(m.channelId);
    if (c != store_.channels.end() && c->second.IsPrivate()) {
        title = c->second.type == chan::DM ? author : store_.ChannelName(m.channelId);
    } else {
        title = "#" + store_.ChannelName(m.channelId);
        if (auto g = store_.guilds.find(m.guildId); g != store_.guilds.end()) title += " (" + g->second.name + ")";
    }
    std::string body = PlainText(m.content, names);
    if (body.empty() && !m.attachments.empty()) body = "sent an attachment";
    if (c == store_.channels.end() || !c->second.IsPrivate() || c->second.type == chan::GroupDM) body = author + ": " + body;
    notices_.push_front({m.guildId, m.channelId, m.authorId, title, body, m.timestamp, false});
    if (notices_.size() > 100) notices_.pop_back();
}

void App::seeNotices(uint64_t channel) {
    for (auto& n : notices_)
        if (!channel || n.channel == channel) n.seen = true;
}

// --- loading and subscriptions ---

void App::open(uint64_t guild, uint64_t channel) {
    if (channel_) drafts_[channel_] = draft_.data();
    commandPicked_.clear();
    commandPick_ = 0;
    if (guild && !channel) {
        channel = lastChannel_.count(guild) ? lastChannel_[guild] : 0;
        if (!store_.channels.count(channel) || !store_.CanView(channel)) channel = 0;
        if (!channel)
            for (uint64_t c : store_.GuildChannels(guild))
                if (store_.channels[c].IsText()) {
                    channel = c;
                    break;
                }
    }
    bool changed = channel != channel_;
    guild_ = guild;
    channel_ = channel;
    if (guild) lastChannel_[guild] = channel;
    config_.guild = guild;
    config_.channel = channel;
    // Direct messages with none picked: show the list to pick from, until
    // back in a server.
    if (!guild && !channel && !config_.channels) config_.channels = listShownForDMs_ = true;
    if (guild && listShownForDMs_) config_.channels = listShownForDMs_ = false;
    SaveConfig(config_);
    if (changed) {
        std::fill(draft_.begin(), draft_.end(), 0);
        if (auto it = drafts_.find(channel); it != drafts_.end())
            strncpy(draft_.data(), it->second.c_str(), draft_.size() - 1);
        draftHash_ = std::hash<std::string>()(draft_.data());  // a restored draft isn't typing
        replyTo_ = editing_ = 0;
        files_.clear();
        composeError_.clear();
        mentionCodes_.clear();
        suggestions_.clear();
        atBottom_ = true;
        firstShown_ = 0;
        video_->Stop();
        gif_->Stop();
        focusComposer_ = true;
    }
    if (!channel) return;
    Messages& ms = store_.messages[channel];
    if (!ms.loaded && !ms.loading) loadLatest(channel);
    if (guild && subscribedChannel_ != channel) {
        // Channels everyone sees alike share a member list, and Discord
        // doesn't send one again that we already have: borrow it.
        if (!listFor_.count(channel)) {
            auto sig = viewSignature(channel);
            for (const auto& [other, id] : listFor_)
                if (store_.channels.count(other) && store_.channels[other].guildId == guild && viewSignature(other) == sig) {
                    listFor_[channel] = id;
                    break;
                }
        }
        subscribe(guild, channel, 1);
    }
}

void App::loadLatest(uint64_t channel) {
    Messages& ms = store_.messages[channel];
    ms.loading = true;
    int gen = generation_;
    Api* api = api_.get();
    fetcher_->Run([this, api, channel, gen] {
        HttpResult r = api->Call("GET", "/channels/" + IdString(channel) + "/messages?limit=50");
        post([this, r, channel, gen] {
            if (gen != generation_) return;
            json page = json::parse(r.body, nullptr, false);
            if (r.ok()) {
                store_.Page(channel, page, 0, 50);
                askMembers(channel);
            } else {
                Messages& ms = store_.messages[channel];
                ms.loading = false;
                ms.loaded = true;
                ms.reachedStart = true;
                if (channel == channel_)
                    composeError_ = r.status == 403 ? "You can't read this channel's history." : "Couldn't load messages: " + r.error;
            }
        });
    });
}

void App::loadOlder(uint64_t channel) {
    Messages& ms = store_.messages[channel];
    if (ms.loadingOlder || ms.reachedStart || ms.list.empty()) return;
    uint64_t before = 0;
    for (const auto& m : ms.list)
        if (!m.pending) {
            before = m.id;
            break;
        }
    if (!before) return;
    ms.loadingOlder = true;
    int gen = generation_;
    Api* api = api_.get();
    fetcher_->Run([this, api, channel, before, gen] {
        HttpResult r = api->Call("GET", "/channels/" + IdString(channel) + "/messages?limit=50&before=" + IdString(before));
        post([this, r, channel, before, gen] {
            if (gen != generation_) return;
            if (r.ok()) {
                store_.Page(channel, json::parse(r.body, nullptr, false), before, 50);
                askMembers(channel);
            } else {
                store_.messages[channel].loadingOlder = false;
            }
        });
    });
}

void App::subscribe(uint64_t guild, uint64_t channel, int ranges) {
    json rs = json::array();
    for (int i = 0; i < ranges; i++) rs.push_back({i * 100, i * 100 + 99});
    json sub = {{"typing", true},
                {"threads", true},
                {"activities", true},
                {"member_updates", false},
                {"channels", {{IdString(channel), rs}}}};
    if (gateway_->Send(37, {{"subscriptions", {{IdString(guild), sub}}}})) {
        subscribedChannel_ = channel;
        listRanges_[channel] = ranges;
    }
}

void App::askMembers(uint64_t channel) {
    auto c = store_.channels.find(channel);
    if (c == store_.channels.end() || !c->second.guildId) return;
    std::vector<uint64_t> missing = store_.MissingMembers(channel);
    for (size_t i = 0; i < missing.size(); i += 100) {
        std::vector<uint64_t> batch(missing.begin() + i, missing.begin() + std::min(missing.size(), i + 100));
        json ids = json::array();
        for (uint64_t u : batch) ids.push_back(IdString(u));
        if (gateway_->Send(8, {{"guild_id", IdString(c->second.guildId)}, {"user_ids", ids}, {"presences", false}}))
            store_.NoteAsked(c->second.guildId, batch);
    }
}

void App::ack(uint64_t channel) {
    auto c = store_.channels.find(channel);
    if (c == store_.channels.end() || !c->second.lastMessageId || !api_) return;
    uint64_t last = c->second.lastMessageId;
    acked_[channel] = GetTickCount64();
    store_.Acked(channel, last);
    Api* api = api_.get();
    sender_->Run([api, channel, last] {
        api->Call("POST", "/channels/" + IdString(channel) + "/messages/" + IdString(last) + "/ack",
                  {{"token", nullptr}});
    });
}

// --- sending ---

std::string App::encodeMentions(const std::string& text) const {
    std::string out = text;
    // Longest first, so "@Sam Smith" goes before "@Sam".
    std::vector<std::pair<std::string, std::string>> codes(mentionCodes_.begin(), mentionCodes_.end());
    std::sort(codes.begin(), codes.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [shown, code] : codes) {
        size_t pos = 0;
        while ((pos = out.find(shown, pos)) != std::string::npos) {
            out.replace(pos, shown.size(), code);
            pos += code.size();
        }
    }
    // :shortcodes: become emoji: the guild's own, else Unicode ones.
    std::string result;
    size_t i = 0;
    const Guild* g = guild_ && store_.guilds.count(guild_) ? &store_.guilds.at(guild_) : nullptr;
    while (i < out.size()) {
        if (out[i] == ':') {
            size_t end = out.find(':', i + 1);
            if (end != std::string::npos && end > i + 1 && end - i < 40) {
                std::string name = out.substr(i + 1, end - i - 1);
                bool word = std::all_of(name.begin(), name.end(), [](char ch) { return isalnum((unsigned char)ch) || ch == '_' || ch == '+' || ch == '-'; });
                if (word) {
                    std::string code;
                    if (g)
                        for (const auto& e : g->emojis)
                            if (e.name == name) {
                                code = std::string(e.animated ? "<a:" : "<:") + e.name + ":" + IdString(e.id) + ">";
                                break;
                            }
                    if (code.empty())
                        if (const EmojiEntry* e = EmojiByName(name)) code = e->emoji;
                    if (!code.empty()) {
                        result += code;
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        result += out[i++];
    }
    return result;
}

void App::send() {
    std::string text = draft_.data();
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    size_t lead = text.find_first_not_of("\n ");
    text = lead == std::string::npos ? "" : text.substr(lead);
    if (editing_) {
        saveEdit();
        return;
    }
    size_t rest = 0;
    if (const Command* c = typedCommand(&rest)) {
        runCommand(*c, rest);
        return;
    }
    if (text.empty() && files_.empty()) return;
    if (!canSend(channel_)) return;
    std::string content = encodeMentions(text);
    if (content.size() > 2000) {
        composeError_ = "That's over Discord's 2000 character limit (" + std::to_string(content.size()) + ").";
        return;
    }
    std::vector<Api::File> files;
    for (auto& f : files_) files.push_back({f.name, std::move(f.bytes), f.type});
    files_.clear();
    std::string nonce = NewNonce();
    Message m;
    m.id = std::strtoull(nonce.c_str(), nullptr, 10);
    m.channelId = channel_;
    m.guildId = guild_;
    m.authorId = store_.me;
    m.content = content;
    m.nonce = nonce;
    m.timestamp = IdTime(m.id);
    if (replyTo_) {
        m.replyTo = replyTo_;
        Message::Reply r;
        for (const auto& x : store_.messages[channel_].list)
            if (x.id == replyTo_) {
                r.authorId = x.authorId;
                r.content = x.content;
            }
        m.reply = r;
    }
    for (const auto& f : files) {
        Attachment a;
        a.filename = f.name;
        a.size = f.bytes.size();
        m.attachments.push_back(a);
    }
    store_.AddPending(m);
    typingSent_ = 0;
    sendMessage(channel_, content, replyTo_, std::move(files), nonce);
    std::fill(draft_.begin(), draft_.end(), 0);
    drafts_.erase(channel_);
    replyTo_ = 0;
    mentionCodes_.clear();
    composeError_.clear();
    atBottom_ = true;
}

void App::sendMessage(uint64_t channel, std::string content, uint64_t replyTo, std::vector<Api::File> files,
                      const std::string& nonce) {
    json payload = {{"content", content}, {"nonce", nonce}, {"tts", false}, {"flags", 0}};
    if (replyTo) {
        payload["message_reference"] = {{"channel_id", IdString(channel)}, {"message_id", IdString(replyTo)}};
        if (guild_) payload["message_reference"]["guild_id"] = IdString(guild_);
    }
    if (!files.empty()) {
        json atts = json::array();
        for (size_t i = 0; i < files.size(); i++) atts.push_back({{"id", std::to_string(i)}, {"filename", files[i].name}});
        payload["attachments"] = atts;
    }
    int gen = generation_;
    Api* api = api_.get();
    std::string path = "/channels/" + IdString(channel) + "/messages";
    sender_->Run([this, api, path, payload, files = std::move(files), channel, nonce, gen] {
        HttpResult r = files.empty() ? api->Call("POST", path, payload) : api->Upload(path, payload, files);
        if (r.ok()) return;  // MESSAGE_CREATE replaces the pending copy
        std::string why = r.error;
        json body = json::parse(r.body, nullptr, false);
        if (body.is_object() && body.contains("message")) why = body["message"].get<std::string>();
        if (r.status == 413) why = "The files are too big for Discord.";
        post([this, channel, nonce, why, gen] {
            if (gen != generation_) return;
            store_.FailPending(channel, nonce);
            if (channel == channel_) composeError_ = "Couldn't send: " + why;
        });
    });
}

const CommandIndex* App::commandsHere() {
    auto it = commandIndex_.find(guild_ ? guild_ : channel_);
    return it == commandIndex_.end() ? nullptr : &it->second;
}

void App::loadCommands() {
    uint64_t key = guild_ ? guild_ : channel_;
    if (!key || !api_ || commandIndex_.count(key) || commandsLoading_.count(key)) return;
    commandsLoading_.insert(key);
    int gen = generation_;
    Api* api = api_.get();
    std::string path = guild_ ? "/guilds/" + IdString(guild_) + "/application-command-index"
                              : "/channels/" + IdString(channel_) + "/application-command-index";
    fetcher_->Run([this, api, path, key, gen] {
        // The server's (or DM's) apps, then the user's own.
        CommandIndex ix;
        for (const std::string& p : {path, std::string("/users/@me/application-command-index")}) {
            HttpResult r = api->Call("GET", p);
            json j = json::parse(r.body, nullptr, false);
            if (r.ok() && j.is_object()) ReadCommandIndex(j, ix);
        }
        post([this, ix = std::move(ix), key, gen]() mutable {
            if (gen != generation_) return;
            commandsLoading_.erase(key);
            commandIndex_[key] = std::move(ix);
        });
    });
}

const Command* App::typedCommand(size_t* rest) {
    if (draft_[0] != '/' || editing_) return nullptr;
    const CommandIndex* ix = commandsHere();
    return ix ? MatchCommand(ix->commands, draft_.data(), commandPick_, rest) : nullptr;
}

json App::interaction(int type, const Command& c, const json& options, const std::string& nonce) const {
    json data = {{"version", c.version},      {"id", IdString(c.id)},          {"name", c.parts[0]},
                 {"type", 1},                 {"options", options},            {"application_command", c.raw},
                 {"attachments", json::array()}};
    json p = {{"type", type},
              {"application_id", IdString(c.applicationId)},
              {"channel_id", IdString(channel_)},
              {"session_id", sessionId_},
              {"data", data},
              {"nonce", nonce}};
    if (guild_) p["guild_id"] = IdString(guild_);
    if (type == 2) p["analytics_location"] = "slash_ui";
    return p;
}

void App::runCommand(const Command& c, size_t rest) {
    std::string typed = draft_.data();
    std::vector<std::pair<std::string, std::string>> values;
    std::string error;
    json options;
    // Mentions picked from suggestions become <@id>, which options read.
    if (!SplitOptions(c, encodeMentions(typed.substr(rest)), &values, &error) ||
        !OptionsJson(c, values, commandPicked_, "", &options, &error)) {
        composeError_ = error;
        return;
    }
    std::string nonce = NewNonce(), path = "/" + c.path;
    json payload = interaction(2, c, options, nonce);
    commandNonces_[nonce] = path;
    commandStatus_ = "Sending " + path + "…";
    commandSentAt_ = now_;
    composeError_.clear();
    std::fill(draft_.begin(), draft_.end(), 0);
    drafts_.erase(channel_);
    mentionCodes_.clear();
    commandPicked_.clear();
    commandPick_ = 0;
    atBottom_ = true;
    int gen = generation_;
    Api* api = api_.get();
    uint64_t channel = channel_;
    sender_->Run([this, api, payload, nonce, path, channel, gen] {
        // Discord's app sends interactions as a form, files or not.
        HttpResult r = api->Upload("/interactions", payload, {});
        if (r.ok()) return;  // INTERACTION_SUCCESS or _FAILURE follows
        std::string why = r.error;
        json body = json::parse(r.body, nullptr, false);
        if (body.is_object() && body.contains("message")) why = body["message"].get<std::string>();
        post([this, nonce, path, why, channel, gen] {
            if (gen != generation_) return;
            commandNonces_.erase(nonce);
            commandStatus_.clear();
            if (channel == channel_) composeError_ = "Couldn't run " + path + ": " + why;
        });
    });
}

void App::requestAutocomplete(const Command& c, const std::string& typed, const std::string& option) {
    std::vector<std::pair<std::string, std::string>> values;
    std::string error;
    json options;
    if (!SplitOptions(c, typed, &values, &error)) return;
    // The option being completed goes even before anything's typed in it.
    if (std::none_of(values.begin(), values.end(), [&](const auto& v) { return v.first == option; })) values.push_back({option, ""});
    if (!OptionsJson(c, values, commandPicked_, option, &options, &error)) return;
    acNonce_ = NewNonce();
    acPendingFor_ = IdString(c.id) + " " + c.path + " " + option;
    json payload = interaction(4, c, options, acNonce_);
    Api* api = api_.get();
    fetcher_->Run([api, payload] { api->Upload("/interactions", payload, {}); });
}

void App::saveEdit() {
    std::string content = encodeMentions(draft_.data());
    uint64_t channel = channel_, id = editing_;
    editing_ = 0;
    std::fill(draft_.begin(), draft_.end(), 0);
    if (auto it = drafts_.find(channel); it != drafts_.end()) {
        strncpy(draft_.data(), it->second.c_str(), draft_.size() - 1);
        drafts_.erase(it);
    }
    for (auto& m : store_.messages[channel].list)
        if (m.id == id && m.content != content) {
            m.content = content;
            Api* api = api_.get();
            sender_->Run([api, channel, id, content] {
                api->Call("PATCH", "/channels/" + IdString(channel) + "/messages/" + IdString(id), {{"content", content}});
            });
        }
}

void App::deleteMessage(uint64_t channel, uint64_t id) {
    // A failed message only ever existed here.
    auto& list = store_.messages[channel].list;
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->id == id && it->pending) {
            list.erase(it);
            return;
        }
    Api* api = api_.get();
    sender_->Run([api, channel, id] { api->Call("DELETE", "/channels/" + IdString(channel) + "/messages/" + IdString(id)); });
}

void App::react(uint64_t channel, uint64_t id, const Emoji& e, bool add) {
    std::string code = e.id ? UrlEncode(e.name + ":" + IdString(e.id)) : UrlEncode(e.name);
    json emoji = {{"id", e.id ? json(IdString(e.id)) : json(nullptr)}, {"name", e.name}};
    // Show it now; the gateway's echo is recognised as ours.
    store_.Apply(add ? "MESSAGE_REACTION_ADD" : "MESSAGE_REACTION_REMOVE",
                 {{"channel_id", IdString(channel)}, {"message_id", IdString(id)}, {"user_id", IdString(store_.me)}, {"emoji", emoji}});
    if (add && !e.id) {
        auto& recent = config_.recentEmoji;
        recent.erase(std::remove(recent.begin(), recent.end(), e.name), recent.end());
        recent.insert(recent.begin(), e.name);
        if (recent.size() > 24) recent.resize(24);
    }
    Api* api = api_.get();
    std::string path = "/channels/" + IdString(channel) + "/messages/" + IdString(id) + "/reactions/" + code +
                       "/@me?location=Message&type=0";
    sender_->Run([api, path, add] { api->Call(add ? "PUT" : "DELETE", path); });
}

void App::attachFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() > (500u << 20)) {
        composeError_ = "That file's too big to send.";
        return;
    }
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    files_.push_back({Narrow(name), std::move(bytes), mimeFor(path)});
    focusComposer_ = true;
}

void App::DropFiles(const std::vector<std::wstring>& paths) {
    if (!channel_ || !canSend(channel_)) return;
    for (const auto& p : paths) attachFile(p);
}

bool App::pasteImage() {
    if (!OpenClipboard(hwnd_)) return false;
    bool done = false;
    static UINT png = RegisterClipboardFormatW(L"PNG");
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        auto drop = static_cast<HDROP>(h);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH];
            if (DragQueryFileW(drop, i, buf, MAX_PATH)) paths.push_back(buf);
        }
        CloseClipboard();
        for (const auto& p : paths) attachFile(p);
        return !paths.empty();
    }
    std::string bytes;
    if (HANDLE h = GetClipboardData(png)) {
        if (void* p = GlobalLock(h)) {
            bytes.assign((const char*)p, GlobalSize(h));
            GlobalUnlock(h);
        }
    } else if (HANDLE dib = GetClipboardData(CF_DIB)) {
        if (void* p = GlobalLock(dib)) {
            bytes = dibToPng(p, GlobalSize(dib));
            GlobalUnlock(dib);
        }
    }
    CloseClipboard();
    if (!bytes.empty()) {
        files_.push_back({"image.png", std::move(bytes), "image/png"});
        done = true;
    }
    return done;
}

// --- helpers ---

std::vector<std::pair<uint64_t, uint64_t>> App::viewSignature(uint64_t channel) const {
    std::vector<std::pair<uint64_t, uint64_t>> sig;
    auto c = store_.channels.find(channel);
    if (c == store_.channels.end()) return sig;
    for (const auto& o : c->second.overwrites) {
        uint64_t bits = (o.allow & perm::ViewChannel ? 1 : 0) | (o.deny & perm::ViewChannel ? 2 : 0);
        if (bits) sig.push_back({o.id, bits});
    }
    std::sort(sig.begin(), sig.end());
    return sig;
}

uint64_t App::perms(uint64_t channel) const { return store_.Permissions(channel, store_.me); }

bool App::canSend(uint64_t channel) const {
    auto c = store_.channels.find(channel);
    if (c == store_.channels.end()) return false;
    if (c->second.IsPrivate()) return true;
    return (perms(channel) & perm::SendMessages) != 0;
}

std::string App::avatarUrl(uint64_t user, uint64_t guild) const {
    if (const Member* m = store_.FindMember(guild, user); m && !m->avatar.empty())
        return "https://cdn.discordapp.com/guilds/" + IdString(guild) + "/users/" + IdString(user) + "/avatars/" + m->avatar +
               ".png?size=64";
    const User* u = store_.FindUser(user);
    if (u && !u->avatar.empty()) return "https://cdn.discordapp.com/avatars/" + IdString(user) + "/" + u->avatar + ".png?size=64";
    return "https://cdn.discordapp.com/embed/avatars/" + std::to_string((user >> 22) % 6) + ".png";
}

ImU32 App::userColour(uint64_t guild, uint64_t user) const {
    uint32_t c = guild ? store_.Colour(guild, user) : 0;
    return c ? U32(Hex(c)) : U32(Colours().heading);
}

ImTextureID App::texture(const std::string& url) {
    const Texture* t = images_->Get(url);
    return t ? ImTextureID((intptr_t)t->srv) : ImTextureID(0);
}

ImTextureID App::image(const std::string& url, bool play) {
    if (play)
        if (const Texture* t = images_->Animated(url, now_)) return ImTextureID((intptr_t)t->srv);
    // The still while it loads, or when it's not playing.
    return texture(url);
}

namespace {

SYSTEMTIME localTime(int64_t ms) {
    ULARGE_INTEGER u;
    u.QuadPart = uint64_t(ms) * 10000 + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                         "August", "September", "October", "November", "December"};
const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

}  // namespace

std::string App::formatTime(int64_t ms, bool full) const {
    SYSTEMTIME t = localTime(ms), today = localTime(nowMs()), yesterday = localTime(nowMs() - 86400000);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
    if (!full) return buf;
    auto same = [](const SYSTEMTIME& a, const SYSTEMTIME& b) {
        return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay;
    };
    if (same(t, today)) return std::string("Today at ") + buf;
    if (same(t, yesterday)) return std::string("Yesterday at ") + buf;
    char date[64];
    snprintf(date, sizeof(date), "%02d/%02d/%04d %s", t.wDay, t.wMonth, t.wYear, buf);
    return date;
}

std::string App::timestampSpan(int64_t seconds, char format) const {
    int64_t ms = seconds * 1000;
    SYSTEMTIME t = localTime(ms);
    char buf[96];
    switch (format) {
    case 't': snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute); break;
    case 'T': snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond); break;
    case 'd': snprintf(buf, sizeof(buf), "%02d/%02d/%04d", t.wDay, t.wMonth, t.wYear); break;
    case 'D': snprintf(buf, sizeof(buf), "%d %s %d", t.wDay, kMonths[t.wMonth - 1], t.wYear); break;
    case 'F':
        snprintf(buf, sizeof(buf), "%s, %d %s %d %02d:%02d", kDays[t.wDayOfWeek], t.wDay, kMonths[t.wMonth - 1], t.wYear,
                 t.wHour, t.wMinute);
        break;
    case 'R': {
        int64_t diff = (ms - nowMs()) / 1000;
        int64_t a = diff < 0 ? -diff : diff;
        const char* unit = "second";
        int64_t n = a;
        if (a >= 31536000) n = a / 31536000, unit = "year";
        else if (a >= 2592000) n = a / 2592000, unit = "month";
        else if (a >= 86400) n = a / 86400, unit = "day";
        else if (a >= 3600) n = a / 3600, unit = "hour";
        else if (a >= 60) n = a / 60, unit = "minute";
        snprintf(buf, sizeof(buf), diff < 0 ? "%lld %s%s ago" : "in %lld %s%s", (long long)n, unit, n == 1 ? "" : "s");
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "%d %s %d %02d:%02d", t.wDay, kMonths[t.wMonth - 1], t.wYear, t.wHour, t.wMinute);
    }
    return buf;
}

Resolver App::resolver(uint64_t guild) {
    Resolver r;
    r.user = [this, guild](uint64_t id) { return store_.DisplayName(guild, id); };
    r.role = [this, guild](uint64_t id) -> std::string {
        if (auto g = store_.guilds.find(guild); g != store_.guilds.end())
            if (auto ro = g->second.roles.find(id); ro != g->second.roles.end()) return ro->second.name;
        return "unknown-role";
    };
    r.roleColour = [this, guild](uint64_t id) -> ImU32 {
        if (auto g = store_.guilds.find(guild); g != store_.guilds.end())
            if (auto ro = g->second.roles.find(id); ro != g->second.roles.end() && ro->second.colour)
                return U32(Hex(ro->second.colour));
        return 0;
    };
    r.channel = [this](uint64_t id) { return store_.channels.count(id) ? store_.ChannelName(id) : std::string("unknown"); };
    r.time = [this](int64_t t, char f) { return timestampSpan(t, f); };
    return r;
}

std::string App::typingLine(uint64_t channel) {
    auto t = typing_.find(channel);
    if (t == typing_.end()) return {};
    unsigned long long now = GetTickCount64();
    std::vector<std::string> names;
    for (auto it = t->second.begin(); it != t->second.end();) {
        if (it->second <= now) {
            it = t->second.erase(it);
            continue;
        }
        names.push_back(store_.DisplayName(guild_, it->first));
        ++it;
    }
    switch (names.size()) {
    case 0: return {};
    case 1: return names[0] + " is typing…";
    case 2: return names[0] + " and " + names[1] + " are typing…";
    case 3: return names[0] + ", " + names[1] + " and " + names[2] + " are typing…";
    default: return "Several people are typing…";
    }
}

std::string App::channelTitle(uint64_t channel) const {
    auto c = store_.channels.find(channel);
    if (c == store_.channels.end()) return {};
    if (c->second.IsPrivate()) return store_.ChannelName(channel);
    return "#" + c->second.name;
}

const Layout& App::layoutFor(const Message& m, float w) {
    Cached& c = layouts_[m.pending ? std::hash<std::string>()(m.nonce) : m.id];
    size_t version = std::hash<std::string>()(m.content);
    if (c.width != w || c.version != version) {
        bool jumbo = false;
        auto paras = Parse(m.content, &jumbo);
        c.layout = LayOut(paras, w, fonts_, resolver(m.guildId), scale_, jumbo, kBody, false);
        c.width = w;
        c.version = version;
    }
    return c.layout;
}

}  // namespace disky
