// Drawing the window: title bar, login, the server rail, channel and DM
// lists, the user panel, the member list, settings and the emoji picker.
// The message view and composer are in ui_chat.cpp.

#include <algorithm>
#include <cmath>

#include "app.h"
#include "qrcodegen/qrcodegen.hpp"

namespace disky {

namespace {

ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (size_t i = 0; i < name.size() && out.size() < 6;) {
        size_t j = i;
        DecodeUtf8(name, j);
        if (name[i] == ' ') {
            start = true;
        } else if (start) {
            out += name.substr(i, j - i);
            start = false;
        }
        i = j;
    }
    return out;
}

ImU32 statusColour(const std::string& s) {
    const Palette& c = Colours();
    if (s == "online") return U32(c.good);
    if (s == "idle") return U32(c.idle);
    if (s == "dnd") return U32(c.dnd);
    return U32(c.muted);
}

}  // namespace

bool App::Busy() const {
    auto playing = [](const std::unique_ptr<VideoPlayer>& v) { return v && v->Active() && !v->Paused() && !v->Ended(); };
    return playing(video_) || playing(gif_) || (images_ && images_->Animating());
}

bool App::InCaption(float x, float y) const {
    if (y >= captionH_) return false;
    for (const auto& r : noDrag_)
        if (r.Contains(ImVec2(x, y))) return false;
    return true;
}

void App::Frame() {
    now_ = ImGui::GetTime();
    // What's on screen counts as seen.
    if (channel_ && GetForegroundWindow() == hwnd_ && !IsIconic(hwnd_) && atBottom_) seeNotices(channel_);
    images_->Tick();
    gifWanted_ = false;
    const Palette& c = Colours();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, c.bg);
    ImGui::Begin("disky", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    titleBar();
    float top = captionH_, h = vp->Size.y - top, w = vp->Size.x;
    if (!api_) {
        login();
    } else if (!ready_) {
        const char* text = status_.empty() ? "Connecting…" : status_.c_str();
        ImVec2 sz = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((w - sz.x) / 2, top + (h - sz.y) / 2));
        ImGui::TextDisabled("%s", text);
    } else {
        // The columns either side are shown or hidden from the title bar.
        float sideW = config_.channels ? 240 * scale_ : 0;
        bool hasMembers = channel_ && (guild_ || (store_.channels.count(channel_) && store_.channels[channel_].type == chan::GroupDM));
        float memW = hasMembers && config_.members && w > 900 * scale_ ? 200 * scale_ : 0;
        if (sideW > 0) sidebar(0, top, sideW - 1, h);
        chat(sideW, top, w - sideW - memW, h);
        if (memW > 0) memberList(w - memW + 1, top, memW - 1, h);
        // Lines between the columns, in the pixel left between them, so the
        // greys don't run together.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        if (sideW > 0) dl->AddRectFilled(ImVec2(o.x + sideW - 1, o.y + top), ImVec2(o.x + sideW, o.y + top + h), U32(c.border));
        if (memW > 0)
            dl->AddRectFilled(ImVec2(o.x + w - memW, o.y + top), ImVec2(o.x + w - memW + 1, o.y + top + h), U32(c.border));
        serverMenu();
        emojiPicker();
        settings();
    }
    notices();
    ImGui::End();
    // A GIF stops once the mouse leaves it, even if it's gone off screen.
    if (!gifWanted_ && gif_->Active() && !gif_->Paused()) gif_->TogglePause();
}

// --- title bar ---

void App::titleBar() {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    captionH_ = 30 * scale_;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + captionH_), U32(c.rail));
    ImGui::PushFont(fonts_.semibold, kSmall);
    float ty = (captionH_ - ImGui::GetFontSize()) / 2;
    ImGui::PopFont();
    noDrag_.clear();

    // On the left: which server (a menu of them), and the channel list.
    float left;
    if (ready_) {
        left = serversButton(o);
        if (barToggle("channels", icon::List, ImVec2(o.x + left, o.y), config_.channels,
                      config_.channels ? (guild_ ? "Hide the channel list" : "Hide the conversation list")
                                       : (guild_ ? "Show the channel list" : "Show the conversation list"))) {
            config_.channels = !config_.channels;
            SaveConfig(config_);
        }
        left += 36 * scale_;
    } else {
        ImGui::PushFont(fonts_.semibold, kSmall);
        dl->AddText(ImVec2(o.x + 12 * scale_, o.y + ty), U32(c.muted), "disky");
        left = 12 * scale_ + ImGui::CalcTextSize("disky").x;
        ImGui::PopFont();
    }

    // Window buttons, the size Windows draws them, with the bell and the
    // member list button beside them.
    float bw = 46 * scale_, right = bw * 3;
    if (ready_) {
        right += bw;
        bell(ImVec2(o.x + w - right, o.y), bw, captionH_);
        auto ch = store_.channels.find(channel_);
        if (ch != store_.channels.end() && (guild_ || ch->second.type == chan::GroupDM)) {
            right += 36 * scale_;
            if (barToggle("members", icon::People, ImVec2(o.x + w - right, o.y), config_.members,
                          config_.members ? "Hide the member list" : "Show the member list")) {
                config_.members = !config_.members;
                SaveConfig(config_);
            }
        }
    }
    if (!status_.empty() && api_) {
        ImGui::PushFont(fonts_.semibold, kSmall);
        ImVec2 sz = ImGui::CalcTextSize(status_.c_str());
        dl->AddText(ImVec2(o.x + (w - sz.x) / 2, o.y + ty), U32(c.idle), status_.c_str());
        ImGui::PopFont();
    } else if (ready_ && channel_) {
        channelHeader(left, right);
    }
    const char* glyphs[] = {icon::Minimise, IsZoomed(hwnd_) ? icon::Restore : icon::Maximise, icon::Close};
    ImGui::PushFont(fonts_.regular, 10);
    for (int i = 0; i < 3; i++) {
        ImVec2 a(o.x + w - bw * (3 - i), o.y), b(a.x + bw, a.y + captionH_);
        noDrag_.push_back(ImRect(ImVec2(a.x - o.x, 0), ImVec2(b.x - o.x, captionH_)));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool pressed = ImGui::InvisibleButton("wb", ImVec2(bw, captionH_));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(a, b, i == 2 ? IM_COL32(196, 43, 28, 255) : U32(c.hover));
        ImVec2 sz = ImGui::CalcTextSize(glyphs[i]);
        dl->AddText(ImVec2(a.x + (bw - sz.x) / 2, a.y + (captionH_ - sz.y) / 2), hov && i == 2 ? IM_COL32_WHITE : U32(c.text),
                    glyphs[i]);
        if (pressed) {
            if (i == 0) ShowWindow(hwnd_, SW_MINIMIZE);
            if (i == 1) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            if (i == 2) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }
    ImGui::PopFont();
}

// --- notifications ---

void App::bell(ImVec2 a, float w, float h) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    noDrag_.push_back(ImRect(ImVec2(a.x - o.x, 0), ImVec2(a.x - o.x + w, h)));
    ImGui::SetCursorScreenPos(a);
    bool pressed = ImGui::InvisibleButton("bell", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    int unseen = 0;
    for (const auto& n : notices_) unseen += !n.seen;
    ImVec2 mid(a.x + w / 2, a.y + h / 2);
    if (hov || noticesOpen_) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(c.hover));
    if (unseen) {
        // A soft glow: rings fading out from the bell. Still, not pulsing,
        // so an unseen one costs nothing while disky waits.
        for (int i = 6; i >= 1; i--)
            dl->AddCircleFilled(mid, (4 + i * 2.2f) * scale_, U32(c.accent, 0.05f), 32);
    }
    ImGui::PushFont(fonts_.regular, 14);
    ImVec2 sz = ImGui::CalcTextSize(icon::Bell);
    dl->AddText(ImVec2(mid.x - sz.x / 2, mid.y - sz.y / 2), unseen ? U32(c.accent) : U32(hov ? c.heading : c.muted), icon::Bell);
    ImGui::PopFont();
    if (unseen) {
        std::string n = unseen > 9 ? "9+" : std::to_string(unseen);
        ImGui::PushFont(fonts_.bold, 10);
        ImVec2 ns = ImGui::CalcTextSize(n.c_str());
        ImVec2 p(mid.x + 5 * scale_, a.y + 4 * scale_);
        float r = std::max(ns.x / 2 + 3 * scale_, 7 * scale_);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, U32(c.bad), 20);
        dl->AddText(ImVec2(p.x + r - ns.x / 2, p.y + r - ns.y / 2), IM_COL32_WHITE, n.c_str());
        ImGui::PopFont();
    }
    if (hov && !noticesOpen_) ImGui::SetTooltip(unseen ? "Notifications (%d new)" : "Notifications", unseen);
    bellAt_ = ImVec2(a.x + w, a.y + h);
    if (pressed) {
        noticesOpen_ = !noticesOpen_;
        if (noticesOpen_) ImGui::OpenPopup("notices");
    }
}

void App::notices() {
    const Palette& c = Colours();
    float pw = 380 * scale_, ph = 460 * scale_;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(std::clamp(bellAt_.x - pw, 8 * scale_, vp.x - pw - 8 * scale_), bellAt_.y + 2 * scale_));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(pw, 0), ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (!ImGui::BeginPopup("notices")) {
        ImGui::PopStyleVar(2);
        // Closed by a click outside, Escape or the bell: all seen now.
        if (noticesOpen_ && !ImGui::IsPopupOpen("notices")) {
            noticesOpen_ = false;
            seeNotices(0);
        }
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad = 12 * scale_;
    ImVec2 o = ImGui::GetCursorScreenPos();
    float headH = 36 * scale_;
    ImGui::PushFont(fonts_.semibold, 14);
    dl->AddText(ImVec2(o.x + pad, o.y + (headH - ImGui::GetFontSize()) / 2), U32(c.heading), "Notifications");
    ImGui::PopFont();
    if (!notices_.empty()) {
        ImGui::PushFont(fonts_.regular, 12);
        const char* t = "Clear all";
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImVec2 ta(o.x + pw - pad - ts.x, o.y + (headH - ts.y) / 2);
        ImGui::SetCursorScreenPos(ImVec2(ta.x - 4 * scale_, o.y + 6 * scale_));
        bool clear = ImGui::InvisibleButton("clear", ImVec2(ts.x + 8 * scale_, headH - 12 * scale_));
        dl->AddText(ta, U32(ImGui::IsItemHovered() ? c.heading : c.muted), t);
        ImGui::PopFont();
        if (clear) notices_.clear();
    }
    dl->AddLine(ImVec2(o.x, o.y + headH - 1), ImVec2(o.x + pw, o.y + headH - 1), U32(c.border));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + headH));

    if (notices_.empty()) {
        ImGui::Dummy(ImVec2(pw, 60 * scale_));
        ImGui::PushFont(fonts_.regular, 13);
        const char* t = "Nothing yet. Mentions and DMs land here.";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(o.x + (pw - ts.x) / 2, o.y + headH + (60 * scale_ - ts.y) / 2), U32(c.muted), t);
        ImGui::PopFont();
    } else {
        // The list scrolls within the panel's height.
        float rowsH = 0;
        std::vector<float> heights;
        float textW = pw - pad * 2 - 36 * scale_;
        for (const auto& n : notices_) {
            ImGui::PushFont(fonts_.regular, 13);
            float bh = std::min(ImGui::CalcTextSize(n.body.c_str(), nullptr, false, textW).y, ImGui::GetFontSize() * 3.9f);
            ImGui::PopFont();
            heights.push_back(std::max(44 * scale_, 26 * scale_ + bh + 8 * scale_));
            rowsH += heights.back();
        }
        float listH = std::min(rowsH, ph - headH);
        ImGui::BeginChild("list", ImVec2(pw, listH), 0);
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        int i = 0;
        for (auto it = notices_.begin(); it != notices_.end(); ++it, ++i) {
            const Notice& n = *it;
            ImVec2 a = ImGui::GetCursorScreenPos();
            float rh = heights[i];
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("n", ImVec2(pw, rh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) {
                ldl->AddRectFilled(a, ImVec2(a.x + pw, a.y + rh), U32(c.hover));
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (!n.seen) ldl->AddRectFilled(a, ImVec2(a.x + 3 * scale_, a.y + rh), U32(c.accent));
            float av = 28 * scale_;
            ImVec2 aa(a.x + pad, a.y + 8 * scale_);
            if (ImTextureID t = texture(avatarUrl(n.author, n.guild)))
                ldl->AddImageRounded(ImTextureRef(t), aa, ImVec2(aa.x + av, aa.y + av), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, av / 2);
            float tx = aa.x + av + 8 * scale_;
            std::string when = formatTime(n.time, false);
            ImGui::PushFont(fonts_.regular, 11);
            ImVec2 ws = ImGui::CalcTextSize(when.c_str());
            ldl->AddText(ImVec2(a.x + pw - pad - ws.x, a.y + 9 * scale_), U32(c.muted), when.c_str());
            ImGui::PopFont();
            ImGui::PushFont(fonts_.semibold, 13);
            Label(ldl, fonts_.semibold, ImGui::GetFontSize(), ImVec2(tx, a.y + 7 * scale_), U32(n.seen ? c.text : c.heading), n.title,
                  textW - ws.x - 8 * scale_, [this](const std::string& u) { return texture(u); });
            ImGui::PopFont();
            ImGui::PushFont(fonts_.regular, 13);
            ldl->PushClipRect(ImVec2(tx, a.y), ImVec2(tx + textW, a.y + rh - 6 * scale_), true);
            ldl->AddText(nullptr, 0, ImVec2(tx, a.y + 26 * scale_), U32(c.muted), n.body.c_str(), nullptr, textW);
            ldl->PopClipRect();
            ImGui::PopFont();
            if (std::next(it) != notices_.end())
                ldl->AddLine(ImVec2(a.x + pad, a.y + rh - 1), ImVec2(a.x + pw - pad, a.y + rh - 1), U32(c.border, 0.5f));
            if (clicked) {
                uint64_t g = n.guild, ch = n.channel;
                ImGui::CloseCurrentPopup();
                open(g, ch);
                break;
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

// --- login ---

void App::login() {
    const Palette& c = Colours();
    ImVec2 o = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    float cardW = 440 * scale_, cardH = 520 * scale_;
    ImVec2 a(o.x + (w - cardW) / 2, o.y + captionH_ + std::max(20 * scale_, (h - captionH_ - cardH) / 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + cardW, a.y + cardH), U32(c.sidebar));
    float pad = 32 * scale_, x = a.x + pad, y = a.y + pad, inner = cardW - pad * 2;

    auto centred = [&](const std::string& text, ImFont* f, float size, ImU32 col) {
        ImGui::PushFont(f, size);
        ImVec2 sz = ImGui::CalcTextSize(text.c_str(), nullptr, false, inner);
        ImGui::SetCursorScreenPos(ImVec2(x + std::max(0.0f, (inner - sz.x) / 2), y));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        ImGui::PushTextWrapPos(x + inner);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        y += sz.y + 8 * scale_;
    };
    centred("Sign in with QR code", fonts_.semibold, kH2, U32(c.heading));

    RemoteAuth::Status s = remote_ ? remote_->Get() : RemoteAuth::Status();
    float qr = 200 * scale_;
    ImVec2 qa(x + (inner - qr) / 2, y + 8 * scale_);
    dl->AddRectFilled(qa, ImVec2(qa.x + qr, qa.y + qr), IM_COL32_WHITE);
    using Stage = RemoteAuth::Status::Stage;
    if (s.stage == Stage::Waiting && !s.qr.empty()) {
        auto code = qrcodegen::QrCode::encodeText(s.qr.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int n = code.getSize();
        float quiet = 12 * scale_, cell = std::floor((qr - quiet * 2) / n);
        float off = (qr - cell * n) / 2;
        for (int yy = 0; yy < n; yy++)
            for (int xx = 0; xx < n; xx++)
                if (code.getModule(xx, yy)) {
                    ImVec2 m(qa.x + off + xx * cell, qa.y + off + yy * cell);
                    dl->AddRectFilled(m, ImVec2(m.x + cell, m.y + cell), IM_COL32_BLACK);
                }
    } else if (s.stage == Stage::Scanned && !s.userId.empty()) {
        std::string url = s.avatar.empty() || s.avatar == "0"
                              ? "https://cdn.discordapp.com/embed/avatars/0.png"
                              : "https://cdn.discordapp.com/avatars/" + s.userId + "/" + s.avatar + ".png?size=128";
        if (ImTextureID t = texture(url)) {
            float av = 96 * scale_;
            ImVec2 m(qa.x + (qr - av) / 2, qa.y + (qr - av) / 2);
            dl->AddImage(ImTextureRef(t), m, ImVec2(m.x + av, m.y + av), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
        }
    }
    y = qa.y + qr + 16 * scale_;
    switch (s.stage) {
    case Stage::Connecting: centred("Getting a code…", fonts_.regular, kBody, U32(c.muted)); break;
    case Stage::Waiting:
        centred("Scan it with the Discord app on your phone: tap your profile picture, then Scan QR Code.", fonts_.regular,
                kBody, U32(c.muted));
        break;
    case Stage::Scanned: centred("Check your phone to finish signing in as " + s.username + ".", fonts_.regular, kBody, U32(c.text)); break;
    case Stage::Done: centred("Signing in…", fonts_.regular, kBody, U32(c.muted)); break;
    case Stage::Failed: {
        centred(s.error, fonts_.regular, kBody, U32(c.bad));
        ImGui::SetCursorScreenPos(ImVec2(x + (inner - 120 * scale_) / 2, y));
        if (ImGui::Button("Try again", ImVec2(120 * scale_, 0)) && remote_) remote_->Start();
        y += ImGui::GetFrameHeight() + 8 * scale_;
        break;
    }
    }
    if (!loginError_.empty()) centred(loginError_, fonts_.regular, kSmall, U32(c.idle));

    y = a.y + cardH - pad - ImGui::GetFrameHeight() * 2 - 12 * scale_;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushFont(fonts_.semibold, kSmall);
    ImGui::TextDisabled("OR PASTE A TOKEN");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(x, y + 22 * scale_));
    ImGui::SetNextItemWidth(inner - 90 * scale_);
    bool enter = ImGui::InputText("##token", tokenBuf_, sizeof(tokenBuf_),
                                  ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
    bool go = ImGui::Button("Sign in", ImVec2(82 * scale_, 0));
    ImGui::PopStyleColor();
    if ((enter || go) && tokenBuf_[0]) {
        std::string token = tokenBuf_;
        token.erase(std::remove_if(token.begin(), token.end(), [](char ch) { return ch == '"' || isspace((unsigned char)ch); }),
                    token.end());
        SecureZeroMemory(tokenBuf_, sizeof(tokenBuf_));
        if (remote_) {
            remote_->Stop();
            remote_.reset();
        }
        signIn(token);
    }
}

// --- rail ---

// guildIcon draws a server's icon (its initials if it has none), or for
// guild 0 the direct messages icon.
void App::guildIcon(ImDrawList* dl, uint64_t gid, ImVec2 a, float size) {
    const Palette& c = Colours();
    ImVec2 b(a.x + size, a.y + size);
    if (!gid) {
        dl->AddRectFilled(a, b, U32(c.accent));
        ImGui::PushFont(fonts_.regular, size * 0.5f / scale_);
        ImVec2 sz = ImGui::CalcTextSize(icon::Chat);
        dl->AddText(ImVec2(a.x + (size - sz.x) / 2, a.y + (size - sz.y) / 2), IM_COL32_WHITE, icon::Chat);
        ImGui::PopFont();
        return;
    }
    const Guild& g = store_.guilds[gid];
    ImTextureID t = g.icon.empty() ? ImTextureID(0)
                                   : texture("https://cdn.discordapp.com/icons/" + IdString(gid) + "/" + g.icon + ".png?size=96");
    if (t) {
        dl->AddImage(ImTextureRef(t), a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
        return;
    }
    dl->AddRectFilled(a, b, U32(c.bg));
    std::string ini = initials(g.name);
    ImGui::PushFont(fonts_.semibold, (ini.size() > 3 ? 0.29f : 0.36f) * size / scale_);
    ImVec2 sz = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(a.x + (size - sz.x) / 2, a.y + (size - sz.y) / 2), U32(c.text), ini.c_str());
    ImGui::PopFont();
}

// mentionBadge draws a red count pill ending at right, centred on y; it
// returns its width.
float App::mentionBadge(ImDrawList* dl, int n, float right, float y) {
    std::string t = n > 99 ? "99+" : std::to_string(n);
    ImGui::PushFont(fonts_.bold, 10);
    ImVec2 sz = ImGui::CalcTextSize(t.c_str());
    float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 15 * scale_;
    ImVec2 a(right - bw, y - bh / 2);
    dl->AddRectFilled(a, ImVec2(right, a.y + bh), U32(Colours().dnd), bh / 2);
    dl->AddText(ImVec2(a.x + (bw - sz.x) / 2, a.y + (bh - sz.y) / 2), IM_COL32_WHITE, t.c_str());
    ImGui::PopFont();
    return bw;
}

float App::serversButton(ImVec2 a) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float h = captionH_, ic = 18 * scale_, pad = 8 * scale_;
    std::string name = guild_ && store_.guilds.count(guild_) ? store_.guilds[guild_].name : "Direct messages";
    ImageFor images = [this](const std::string& u) { return texture(u); };
    ImGui::PushFont(fonts_.semibold, kSmall);
    float nameW = std::min(Label(nullptr, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0, 0), 0, name, 0, images), 200 * scale_);
    ImGui::PopFont();
    // Mentions in servers other than this one, so they're not missed.
    int elsewhere = 0;
    for (uint64_t gid : store_.GuildsInOrder())
        if (gid != guild_) elsewhere += store_.GuildMentions(gid);
    if (guild_)
        for (uint64_t ch : store_.PrivateChannels()) elsewhere += store_.Mentions(ch);
    float w = pad + ic + 8 * scale_ + nameW + 8 * scale_ + 10 * scale_ + pad + (elsewhere ? 24 * scale_ : 0);
    noDrag_.push_back(ImRect(ImVec2(a.x - o.x, 0), ImVec2(a.x - o.x + w, h)));
    ImGui::SetCursorScreenPos(a);
    bool pressed = ImGui::InvisibleButton("servers", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (hov || ImGui::IsPopupOpen("servermenu")) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(c.hover));
    guildIcon(dl, guild_, ImVec2(a.x + pad, a.y + (h - ic) / 2), ic);
    float tx = a.x + pad + ic + 8 * scale_;
    ImGui::PushFont(fonts_.semibold, kSmall);
    Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, a.y + (h - ImGui::GetFontSize()) / 2), U32(c.heading), name, nameW,
          images);
    ImGui::PopFont();
    ImGui::PushFont(fonts_.regular, 8);
    ImVec2 cs = ImGui::CalcTextSize(icon::Down);
    dl->AddText(ImVec2(tx + nameW + 8 * scale_, a.y + (h - cs.y) / 2), U32(c.muted), icon::Down);
    ImGui::PopFont();
    if (elsewhere) mentionBadge(dl, elsewhere, a.x + w - pad, a.y + h / 2);
    if (hov) ImGui::SetTooltip("Servers");
    if (pressed) {
        serversAt_ = ImVec2(a.x, a.y + h);
        ImGui::OpenPopup("servermenu");
    }
    return w;
}

void App::serverMenu() {
    const Palette& c = Colours();
    float mw = 280 * scale_, row = 36 * scale_, ic = 24 * scale_, pad = 10 * scale_;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(serversAt_.x, serversAt_.y + 2 * scale_));
    ImGui::SetNextWindowSizeConstraints(ImVec2(mw, 0), ImVec2(mw, vp.y - serversAt_.y - 16 * scale_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4 * scale_));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (!ImGui::BeginPopup("servermenu")) {
        ImGui::PopStyleVar(2);
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImageFor images = [this](const std::string& u) { return texture(u); };
    auto item = [&](uint64_t gid, const std::string& name, bool unread, int mentions) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(gid ? IdString(gid).c_str() : "dms");
        bool clicked = ImGui::InvisibleButton("s", ImVec2(w, row));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool selected = gid == guild_;
        if (selected || hov) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + row), U32(selected ? c.selected : c.hover));
        if (selected) dl->AddRectFilled(a, ImVec2(a.x + 3 * scale_, a.y + row), U32(c.accent));
        guildIcon(dl, gid, ImVec2(a.x + pad, a.y + (row - ic) / 2), ic);
        float right = a.x + w - pad;
        if (mentions)
            right -= mentionBadge(dl, mentions, right, a.y + row / 2) + 6 * scale_;
        else if (unread)
            dl->AddCircleFilled(ImVec2(right - 4 * scale_, a.y + row / 2), 3.5f * scale_, U32(c.heading)), right -= 14 * scale_;
        float tx = a.x + pad + ic + 10 * scale_;
        ImGui::PushFont(unread || selected ? fonts_.semibold : fonts_.regular, 14);
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, a.y + (row - ImGui::GetFontSize()) / 2),
              U32(unread || selected || hov ? c.heading : c.muted), name, right - tx, images);
        ImGui::PopFont();
        if (clicked) {
            ImGui::CloseCurrentPopup();
            open(gid, 0);
        }
    };
    int dmMentions = 0;
    for (uint64_t ch : store_.PrivateChannels()) dmMentions += store_.Mentions(ch);
    item(0, "Direct messages", false, dmMentions);
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x + pad, p.y + 4 * scale_), ImVec2(p.x + mw - pad, p.y + 4 * scale_), U32(c.border));
    ImGui::Dummy(ImVec2(1, 9 * scale_));
    for (uint64_t gid : store_.GuildsInOrder()) {
        const Guild& g = store_.guilds[gid];
        if (g.unavailable) continue;
        item(gid, g.name, store_.GuildUnread(gid), store_.GuildMentions(gid));
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

bool App::barToggle(const char* id, const char* glyph, ImVec2 a, bool on, const char* tip) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float w = 36 * scale_, h = captionH_;
    noDrag_.push_back(ImRect(ImVec2(a.x - o.x, 0), ImVec2(a.x - o.x + w, h)));
    ImGui::SetCursorScreenPos(a);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (hov) {
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(c.hover));
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PushFont(fonts_.regular, 14);
    ImVec2 sz = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(a.x + (w - sz.x) / 2, a.y + (h - sz.y) / 2), U32(hov || on ? c.heading : c.muted), glyph);
    ImGui::PopFont();
    return pressed;
}

// --- sidebar ---

void App::sidebar(float x, float y, float w, float h) {
    const Palette& c = Colours();
    // No header: the server's name is in the title bar.
    float panel = 52 * scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.sidebar);
    ImGui::BeginChild("side", ImVec2(w, h), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("list", ImVec2(w, h - panel), 0);
    ImGui::Dummy(ImVec2(1, 8 * scale_));
    if (guild_)
        channelList(w);
    else
        dmList(w);
    ImGui::Dummy(ImVec2(1, 8 * scale_));
    ImGui::EndChild();
    userPanel(0, h - panel, w);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::channelList(float w) {
    ImageFor images = [this](const std::string& u) { return texture(u); };
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float row = 24 * scale_, side = 6 * scale_;
    for (uint64_t id : store_.GuildChannels(guild_)) {
        const Channel& ch = store_.channels[id];
        if (ch.type == chan::Category) {
            ImGui::Dummy(ImVec2(1, 6 * scale_));
            std::string name = ch.name;
            std::transform(name.begin(), name.end(), name.begin(), ::toupper);
            ImGui::SetCursorPosX(side + 6 * scale_);
            ImGui::PushFont(fonts_.semibold, 11);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::PopFont();
            continue;
        }
        bool selected = id == channel_;
        bool muted = store_.ChannelMuted(id);
        bool unread = store_.Unread(id) && !muted;
        int mentions = store_.Mentions(id);
        ImGui::SetCursorPosX(side);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w - side * 2, a.y + row);
        ImGui::PushID(int(id % 0x7FFFFFFF));
        bool clicked = ImGui::InvisibleButton("ch", ImVec2(w - side * 2, row));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (selected || hov) dl->AddRectFilled(a, b, U32(selected ? c.selected : c.hover));
        if (unread && !selected)
            dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, a.y + row / 2 - 3 * scale_),
                              ImVec2(ImGui::GetWindowPos().x + 3 * scale_, a.y + row / 2 + 3 * scale_), U32(c.heading));
        ImU32 col = selected || unread ? U32(c.heading) : hov ? U32(c.text) : U32(c.muted, muted ? 0.5f : 1.0f);
        ImGui::PushFont(fonts_.regular, 15);
        float hashY = a.y + (row - ImGui::GetFontSize()) / 2;
        dl->AddText(ImVec2(a.x + 6 * scale_, hashY), U32(c.muted, muted ? 0.5f : 1.0f), ch.type == chan::News ? "\xE2\x80\xBC" : "#");
        ImGui::PopFont();
        ImGui::PushFont(unread || selected ? fonts_.semibold : fonts_.regular, 14);
        float right = mentions ? 28 * scale_ : 4 * scale_;
        float avail = b.x - right - (a.x + 22 * scale_);
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 22 * scale_, a.y + (row - ImGui::GetFontSize()) / 2), col,
              ch.name, avail, images);
        ImGui::PopFont();
        if (mentions) {
            std::string n = std::to_string(mentions);
            ImGui::PushFont(fonts_.bold, 12);
            ImVec2 sz = ImGui::CalcTextSize(n.c_str());
            float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 16 * scale_;
            ImVec2 ba(b.x - bw - 8 * scale_, a.y + (row - bh) / 2);
            dl->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), U32(c.dnd));
            dl->AddText(ImVec2(ba.x + (bw - sz.x) / 2, ba.y + (bh - sz.y) / 2), IM_COL32_WHITE, n.c_str());
            ImGui::PopFont();
        }
        if (hov && !ch.topic.empty()) ImGui::SetItemTooltip("%s", ch.topic.substr(0, 200).c_str());
        if (clicked) open(guild_, id);
    }
}

void App::dmList(float w) {
    ImageFor images = [this](const std::string& u) { return texture(u); };
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float row = 34 * scale_, side = 8 * scale_, av = 24 * scale_;
    ImGui::SetCursorPosX(side + 8 * scale_);
    ImGui::PushFont(fonts_.semibold, 12);
    ImGui::TextDisabled("DIRECT MESSAGES");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1, 4 * scale_));
    for (uint64_t id : store_.PrivateChannels()) {
        const Channel& ch = store_.channels[id];
        bool selected = id == channel_;
        bool unread = store_.Unread(id) && !store_.ChannelMuted(id);
        int mentions = store_.Mentions(id);
        ImGui::SetCursorPosX(side);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w - side * 2, a.y + row);
        ImGui::PushID(int(id % 0x7FFFFFFF));
        bool clicked = ImGui::InvisibleButton("dm", ImVec2(w - side * 2, row));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (selected || hov) dl->AddRectFilled(a, b, U32(selected ? c.selected : c.hover));
        ImVec2 ap(a.x + 8 * scale_, a.y + (row - av) / 2);
        if (ch.type == chan::DM && !ch.recipients.empty()) {
            avatar(ch.recipients[0], 0, ap, av, true);
        } else {
            ImTextureID t = ch.icon.empty() ? ImTextureID(0)
                                            : texture("https://cdn.discordapp.com/channel-icons/" + IdString(id) + "/" + ch.icon +
                                                      ".png?size=64");
            if (t)
                dl->AddImage(ImTextureRef(t), ap, ImVec2(ap.x + av, ap.y + av), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
            else {
                dl->AddRectFilled(ap, ImVec2(ap.x + av, ap.y + av), U32(c.accent));
                ImGui::PushFont(fonts_.regular, 16);
                ImVec2 sz = ImGui::CalcTextSize(icon::People);
                dl->AddText(ImVec2(ap.x + (av - sz.x) / 2, ap.y + (av - sz.y) / 2), IM_COL32_WHITE, icon::People);
                ImGui::PopFont();
            }
        }
        std::string name = store_.ChannelName(id);
        ImGui::PushFont(unread || selected ? fonts_.semibold : fonts_.regular, kBody);
        float tx = ap.x + av + 12 * scale_, avail = b.x - tx - (mentions ? 28 * scale_ : 4 * scale_);
        float ty = a.y + (row - ImGui::GetFontSize()) / 2;
        if (ch.type == chan::GroupDM) ty -= 6 * scale_;
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ty),
              selected || unread ? U32(c.heading) : U32(hov ? c.text : c.muted), name, avail, images);
        ImGui::PopFont();
        if (ch.type == chan::GroupDM) {
            ImGui::PushFont(fonts_.regular, 11);
            std::string n = std::to_string(ch.recipients.size() + 1) + " members";
            dl->AddText(ImVec2(tx, ty + 16 * scale_), U32(c.muted), n.c_str());
            ImGui::PopFont();
        }
        if (mentions) {
            std::string n = std::to_string(mentions);
            ImGui::PushFont(fonts_.bold, 12);
            ImVec2 sz = ImGui::CalcTextSize(n.c_str());
            float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 16 * scale_;
            ImVec2 ba(b.x - bw - 8 * scale_, a.y + (row - bh) / 2);
            dl->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), U32(c.dnd));
            dl->AddText(ImVec2(ba.x + (bw - sz.x) / 2, ba.y + (bh - sz.y) / 2), IM_COL32_WHITE, n.c_str());
            ImGui::PopFont();
        }
        if (clicked) open(0, id);
        ImGui::Dummy(ImVec2(1, 2 * scale_));
    }
}

void App::userPanel(float x, float y, float w) {
    const Palette& c = Colours();
    float h = 52 * scale_, av = 32 * scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(c.rail, 0.6f));
    avatar(store_.me, 0, ImVec2(a.x + 8 * scale_, a.y + (h - av) / 2), av, true);
    const User* me = store_.FindUser(store_.me);
    ImGui::PushFont(fonts_.semibold, 14);
    dl->AddText(ImVec2(a.x + 48 * scale_, a.y + 8 * scale_), U32(c.heading), me ? me->Name().c_str() : "");
    ImGui::PopFont();
    ImGui::PushFont(fonts_.regular, 12);
    dl->AddText(ImVec2(a.x + 48 * scale_, a.y + 27 * scale_), U32(c.muted), me ? me->username.c_str() : "");
    ImGui::PopFont();
    float bs = 32 * scale_;
    ImGui::SetCursorPos(ImVec2(x + w - bs - 8 * scale_, y + (h - bs) / 2));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushFont(fonts_.regular, 18);
    if (ImGui::Button(icon::Settings, ImVec2(bs, bs))) {
        settingsOpen_ = true;
        startChoice_ = StartsWithWindows();
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Settings");
}

void App::avatar(uint64_t user, uint64_t guild, ImVec2 at, float size, bool status) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImTextureID t = texture(avatarUrl(user, guild)))
        dl->AddImage(ImTextureRef(t), at, ImVec2(at.x + size, at.y + size));
    else
        dl->AddRectFilled(at, ImVec2(at.x + size, at.y + size), U32(Colours().selected));
    if (!status) return;
    auto p = store_.presence.find(user);
    std::string s = p == store_.presence.end() ? "" : p->second;
    if (s.empty() || s == "offline" || s == "invisible") return;
    // A small square on the corner, edged in the background.
    float d = std::max(5 * scale_, size * 0.32f), edge = std::max(1.0f, scale_);
    ImVec2 a(at.x + size - d + edge, at.y + size - d + edge);
    dl->AddRectFilled(ImVec2(a.x - edge, a.y - edge), ImVec2(a.x + d, a.y + d), U32(Colours().sidebar));
    dl->AddRectFilled(a, ImVec2(a.x + d - edge, a.y + d - edge), statusColour(s));
}

// --- member list ---

void App::memberList(float x, float y, float w, float h) {
    ImageFor images = [this](const std::string& u) { return texture(u); };
    const Palette& c = Colours();
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.sidebar);
    ImGui::BeginChild("members", ImVec2(w, h), 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float row = 22 * scale_, head = 26 * scale_, av = 16 * scale_, side = 6 * scale_;
    ImGui::Dummy(ImVec2(1, 6 * scale_));

    auto member = [&](uint64_t uid, const std::string& status) {
        ImGui::SetCursorPosX(side);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::PushID(int(uid % 0x7FFFFFFF));
        ImGui::InvisibleButton("m", ImVec2(w - side * 2, row));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(a, ImVec2(a.x + w - side * 2, a.y + row), U32(c.hover));
        bool offline = status.empty() || status == "offline";
        if (offline) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        avatar(uid, guild_, ImVec2(a.x + 6 * scale_, a.y + (row - av) / 2), av, true);
        std::string name = store_.DisplayName(guild_, uid);
        ImGui::PushFont(fonts_.regular, 14);
        float tx = a.x + av + 12 * scale_, avail = a.x + w - side * 2 - tx - 4 * scale_;
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, a.y + (row - ImGui::GetFontSize()) / 2),
              U32(ImGui::ColorConvertU32ToFloat4(userColour(guild_, uid)), offline ? 0.4f : 1.0f), name, avail, images);
        ImGui::PopFont();
        if (offline) ImGui::PopStyleVar();
        if (const User* u = store_.FindUser(uid); hov && u) ImGui::SetTooltip("%s", u->username.c_str());
    };

    auto ch = store_.channels.find(channel_);
    if (ch != store_.channels.end() && ch->second.type == chan::GroupDM) {
        ImGui::SetCursorPosX(side + 8 * scale_);
        ImGui::PushFont(fonts_.semibold, 12);
        ImGui::TextDisabled("MEMBERS — %d", int(ch->second.recipients.size() + 1));
        ImGui::PopFont();
        member(store_.me, "online");
        for (uint64_t u : ch->second.recipients) member(u, store_.presence.count(u) ? store_.presence[u] : "");
    } else if (guild_) {
        auto lid = listFor_.find(channel_);
        Guild& g = store_.guilds[guild_];
        if (lid == listFor_.end() || !g.lists.count(lid->second)) {
            ImGui::SetCursorPosX(16 * scale_);
            ImGui::TextDisabled("Loading members…");
        } else {
            const MemberList& list = g.lists[lid->second];
            // Rows only for what's been sent; past it, space for the rest.
            float yy = ImGui::GetCursorPosY();
            float scroll = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
            size_t loaded = 0;
            for (size_t i = 0; i < list.items.size(); i++) {
                const auto& it = list.items[i];
                if (it.group.empty() && !it.userId) continue;
                loaded = i + 1;
                float rh = it.group.empty() ? row : head;
                if (yy + rh < scroll - 100 || yy > scroll + view + 100) {
                    yy += rh;
                    continue;
                }
                ImGui::SetCursorPosY(yy);
                if (!it.group.empty()) {
                    std::string name = it.group == "online" ? "Online" : it.group == "offline" ? "Offline" : "Role";
                    if (auto r = g.roles.find(ParseId(json(it.group))); r != g.roles.end()) name = r->second.name;
                    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
                    ImGui::SetCursorPos(ImVec2(side + 6 * scale_, yy + 10 * scale_));
                    ImGui::PushFont(fonts_.semibold, 11);
                    int count = it.count;
                    if (auto gc = list.groupCounts.find(it.group); gc != list.groupCounts.end()) count = gc->second;
                    ImGui::TextDisabled("%s — %d", name.c_str(), count);
                    ImGui::PopFont();
                } else {
                    member(it.userId, it.status);
                }
                yy += rh;
            }
            ImGui::SetCursorPosY(yy);
            ImGui::Dummy(ImVec2(1, 8 * scale_));
            // Near the end of what's loaded: ask for the next hundred.
            int ranges = listRanges_[channel_];
            if (scroll + view > yy - 400 * scale_ && loaded >= size_t(ranges) * 100 - 5 &&
                int(list.items.size()) < list.memberCount + 200 && ranges < 10)
                subscribe(guild_, channel_, ranges + 1);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// --- settings ---

void App::settings() {
    const Palette& c = Colours();
    if (settingsOpen_) {
        ImGui::OpenPopup("Settings");
        settingsOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(420 * scale_, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * scale_, 16 * scale_));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale_, 10 * scale_));
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(fonts_.semibold, kH2);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();
        if (const User* me = store_.FindUser(store_.me)) ImGui::TextDisabled("Signed in as %s", me->username.c_str());
        if (ImGui::Checkbox("Collect mentions and DMs under the bell", &config_.notify)) SaveConfig(config_);
        ImGui::TextDisabled("Discord's own mute and notification settings apply too.");
        if (ImGui::Checkbox("Start with Windows", &startChoice_)) SetStartWithWindows(startChoice_);
        if (ImGui::Checkbox("Show the member list", &config_.members)) SaveConfig(config_);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, c.bad);
        if (ImGui::Button("Sign out")) {
            ImGui::CloseCurrentPopup();
            signOut();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 * scale_ + ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Done", ImVec2(60 * scale_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// --- emoji picker ---

void App::emojiPicker() {
    const Palette& c = Colours();
    if (pickerOpen_) {
        ImGui::OpenPopup("emoji");
        pickerOpen_ = false;
        pickerSearch_[0] = 0;
    }
    float w = 352 * scale_, h = 400 * scale_;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImVec2 at(std::clamp(pickerAt_.x - w, 8 * scale_, vp.x - w - 8 * scale_), std::clamp(pickerAt_.y - h, captionH_ + 8 * scale_, vp.y - h - 8 * scale_));
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale_, 10 * scale_));
    if (!ImGui::BeginPopup("emoji")) {
        ImGui::PopStyleVar();
        return;
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Find an emoji", pickerSearch_, sizeof(pickerSearch_));
    std::string q = pickerSearch_;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    struct Cell {
        std::string url, name;
        Emoji emoji;
    };
    std::vector<std::pair<std::string, std::vector<Cell>>> sections;
    auto unicode = [](const EmojiEntry& e) {
        return Cell{"https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + TwemojiName(e.emoji) + ".png",
                    FirstName(e), Emoji{0, e.emoji, false}};
    };
    if (q.empty() && !config_.recentEmoji.empty()) {
        std::vector<Cell> recent;
        for (const auto& r : config_.recentEmoji)
            for (const auto& e : EmojiTable())
                if (r == e.emoji) {
                    recent.push_back(unicode(e));
                    break;
                }
        sections.push_back({"Recent", recent});
    }
    if (guild_ && store_.guilds.count(guild_)) {
        std::vector<Cell> mine;
        for (const auto& e : store_.guilds[guild_].emojis) {
            std::string lname = e.name;
            std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
            if (!q.empty() && lname.find(q) == std::string::npos) continue;
            mine.push_back({"https://cdn.discordapp.com/emojis/" + IdString(e.id) + ".png?size=48", e.name, e});
        }
        if (!mine.empty()) sections.push_back({store_.guilds[guild_].name, mine});
    }
    std::vector<std::vector<Cell>> cats(9);
    for (const auto& e : EmojiTable()) {
        if (!q.empty() && std::string(e.names).find(q) == std::string::npos) continue;
        cats[e.category].push_back(unicode(e));
    }
    for (int i = 0; i < 9; i++)
        if (!cats[i].empty()) sections.push_back({kEmojiCategories[i], std::move(cats[i])});

    ImGui::BeginChild("grid", ImVec2(0, 0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cell = 40 * scale_, img = 30 * scale_;
    int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / cell));
    float scroll = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
    const Cell* picked = nullptr;
    std::string hoverName;
    for (const auto& [title, cells] : sections) {
        ImGui::PushFont(fonts_.semibold, 12);
        std::string t = title;
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);
        ImGui::TextDisabled("%s", t.c_str());
        ImGui::PopFont();
        float top = ImGui::GetCursorPosY();
        int rows = int((cells.size() + cols - 1) / cols);
        for (size_t i = 0; i < cells.size(); i++) {
            float y = top + (i / cols) * cell, x = (i % cols) * cell;
            if (y + cell < scroll || y > scroll + view) continue;
            ImGui::SetCursorPos(ImVec2(x, y));
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::PushID(int(i));
            ImGui::PushID(title.c_str());
            if (ImGui::InvisibleButton("e", ImVec2(cell, cell))) picked = &cells[i];
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImGui::PopID();
            if (hov) {
                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), U32(c.hover));
                hoverName = ":" + cells[i].name + ":";
            }
            ImVec2 ia(a.x + (cell - img) / 2, a.y + (cell - img) / 2);
            if (ImTextureID tex = texture(cells[i].url)) dl->AddImage(ImTextureRef(tex), ia, ImVec2(ia.x + img, ia.y + img));
        }
        ImGui::SetCursorPosY(top + rows * cell + 6 * scale_);
    }
    if (sections.empty()) ImGui::TextDisabled("No emoji match.");
    ImGui::EndChild();
    if (!hoverName.empty()) ImGui::SetTooltip("%s", hoverName.c_str());
    if (picked) {
        if (pickerMessage_) {
            bool have = false;
            for (const auto& m : store_.messages[channel_].list)
                if (m.id == pickerMessage_)
                    for (const auto& r : m.reactions)
                        if (r.emoji == picked->emoji && r.me) have = true;
            react(channel_, pickerMessage_, picked->emoji, !have);
        } else {
            std::string add = picked->emoji.id ? ":" + picked->emoji.name + ":" : picked->emoji.name;
            std::string text = draft_.data();
            if (!text.empty() && text.back() != ' ') text += ' ';
            text += add;
            strncpy(draft_.data(), text.c_str(), draft_.size() - 1);
            focusComposer_ = true;
            if (!picked->emoji.id) {
                auto& recent = config_.recentEmoji;
                recent.erase(std::remove(recent.begin(), recent.end(), picked->emoji.name), recent.end());
                recent.insert(recent.begin(), picked->emoji.name);
                if (recent.size() > 24) recent.resize(24);
            }
        }
        if (!ImGui::GetIO().KeyShift) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

}  // namespace disky
