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

bool App::Busy() const { return video_ && video_->Active() && !video_->Paused() && !video_->Ended(); }

bool App::InCaption(float x, float y) const {
    if (y >= captionH_) return false;
    for (const auto& r : noDrag_)
        if (r.Contains(ImVec2(x, y))) return false;
    return true;
}

void App::Frame() {
    now_ = ImGui::GetTime();
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
        float railW = 72 * scale_, sideW = 240 * scale_;
        bool members = config_.members && channel_ &&
                       (guild_ || (store_.channels.count(channel_) && store_.channels[channel_].type == chan::GroupDM));
        float memW = members && w > 900 * scale_ ? 240 * scale_ : 0;
        rail(0, top, h);
        sidebar(railW, top, sideW, h);
        chat(railW + sideW, top, w - railW - sideW - memW, h);
        if (memW > 0) memberList(w - memW, top, memW, h);
        emojiPicker();
        settings();
    }
    ImGui::End();
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
    dl->AddText(ImVec2(o.x + 12 * scale_, o.y + ty), U32(c.muted), "disky");
    std::string mid = ready_ && channel_ ? channelTitle(channel_) : "";
    if (!status_.empty() && api_) mid = status_;
    if (!mid.empty()) {
        ImVec2 sz = ImGui::CalcTextSize(mid.c_str());
        dl->AddText(ImVec2(o.x + (w - sz.x) / 2, o.y + ty), U32(status_.empty() ? c.muted : c.idle), mid.c_str());
    }
    ImGui::PopFont();

    // Window buttons, the size Windows draws them.
    noDrag_.clear();
    float bw = 46 * scale_;
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

// --- login ---

void App::login() {
    const Palette& c = Colours();
    ImVec2 o = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    float cardW = 440 * scale_, cardH = 520 * scale_;
    ImVec2 a(o.x + (w - cardW) / 2, o.y + captionH_ + std::max(20 * scale_, (h - captionH_ - cardH) / 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + cardW, a.y + cardH), U32(c.sidebar), 8 * scale_);
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
    dl->AddRectFilled(qa, ImVec2(qa.x + qr, qa.y + qr), IM_COL32_WHITE, 6 * scale_);
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
            dl->AddImageRounded(ImTextureRef(t), m, ImVec2(m.x + av, m.y + av), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, av / 2);
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

void App::rail(float x, float y, float h) {
    const Palette& c = Colours();
    float w = 72 * scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.rail);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0);
    ImGui::BeginChild("rail", ImVec2(w, h), 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float icon = 48 * scale_, gap = 8 * scale_;
    float cy = 12 * scale_;

    // item draws one rail entry; it returns whether it was clicked.
    auto item = [&](const char* id, bool selected, bool unread, int mentions, const std::string& tip,
                    const std::function<void(ImVec2, ImVec2, float)>& face) {
        ImGui::SetCursorPos(ImVec2((w - icon) / 2, cy));
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + icon, a.y + icon);
        bool clicked = ImGui::InvisibleButton(id, ImVec2(icon, icon));
        bool hov = ImGui::IsItemHovered();
        float rounding = selected || hov ? 16 * scale_ : icon / 2;
        face(a, b, rounding);
        // The pill on the left: tall when selected, short when hovered, a
        // dot when unread.
        float ph = selected ? 40 * scale_ : hov ? 20 * scale_ : unread ? 8 * scale_ : 0;
        if (ph > 0) {
            float px = ImGui::GetWindowPos().x;
            float my = (a.y + b.y) / 2;
            dl->AddRectFilled(ImVec2(px, my - ph / 2), ImVec2(px + 4 * scale_, my + ph / 2), U32(c.heading), 4 * scale_,
                              ImDrawFlags_RoundCornersRight);
        }
        if (mentions > 0) {
            std::string n = mentions > 99 ? "99+" : std::to_string(mentions);
            ImGui::PushFont(fonts_.bold, 12);
            ImVec2 sz = ImGui::CalcTextSize(n.c_str());
            float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 16 * scale_;
            ImVec2 ba(b.x - bw + 2 * scale_, b.y - bh + 2 * scale_);
            dl->AddRectFilled(ImVec2(ba.x - 3 * scale_, ba.y - 3 * scale_), ImVec2(ba.x + bw + 3 * scale_, ba.y + bh + 3 * scale_),
                              U32(c.rail), bh);
            dl->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), U32(c.dnd), bh);
            dl->AddText(ImVec2(ba.x + (bw - sz.x) / 2, ba.y + (bh - sz.y) / 2), IM_COL32_WHITE, n.c_str());
            ImGui::PopFont();
        }
        if (hov && !tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        cy += icon + gap;
        return clicked;
    };

    int dmMentions = 0;
    bool dmUnread = false;
    for (uint64_t ch : store_.PrivateChannels()) {
        dmMentions += store_.Mentions(ch);
        dmUnread |= store_.Unread(ch) && !store_.ChannelMuted(ch);
    }
    if (item("home", guild_ == 0, false, dmMentions, "Direct messages", [&](ImVec2 a, ImVec2 b, float r) {
            dl->AddRectFilled(a, b, guild_ == 0 ? U32(c.accent) : U32(c.bg), r);
            ImGui::PushFont(fonts_.regular, 22);
            ImVec2 sz = ImGui::CalcTextSize(icon::Chat);
            dl->AddText(ImVec2(a.x + (icon - sz.x) / 2, a.y + (icon - sz.y) / 2), guild_ == 0 ? IM_COL32_WHITE : U32(c.text),
                        icon::Chat);
            ImGui::PopFont();
        }))
        open(0, 0);
    (void)dmUnread;
    dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x + w / 2 - 16 * scale_, ImGui::GetWindowPos().y + cy - gap / 2 - 1),
                      ImVec2(ImGui::GetWindowPos().x + w / 2 + 16 * scale_, ImGui::GetWindowPos().y + cy - gap / 2 + 1),
                      U32(c.border));
    cy += gap / 2;

    for (uint64_t gid : store_.GuildsInOrder()) {
        const Guild& g = store_.guilds[gid];
        if (g.unavailable) continue;
        std::string id = "g" + IdString(gid);
        if (item(id.c_str(), guild_ == gid, store_.GuildUnread(gid), store_.GuildMentions(gid), g.name,
                 [&](ImVec2 a, ImVec2 b, float r) {
                     ImTextureID t = g.icon.empty() ? ImTextureID(0)
                                                    : texture("https://cdn.discordapp.com/icons/" + IdString(gid) + "/" +
                                                              g.icon + ".png?size=96");
                     if (t) {
                         dl->AddImageRounded(ImTextureRef(t), a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
                     } else {
                         dl->AddRectFilled(a, b, guild_ == gid ? U32(c.accent) : U32(c.bg), r);
                         std::string ini = initials(g.name);
                         ImGui::PushFont(fonts_.semibold, ini.size() > 3 ? 12.0f : 15.0f);
                         ImVec2 sz = ImGui::CalcTextSize(ini.c_str());
                         dl->AddText(ImVec2(a.x + (icon - sz.x) / 2, a.y + (icon - sz.y) / 2), U32(c.text), ini.c_str());
                         ImGui::PopFont();
                     }
                 }))
            open(gid, 0);
    }
    ImGui::SetCursorPos(ImVec2(0, cy));
    ImGui::Dummy(ImVec2(1, 4 * scale_));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// --- sidebar ---

void App::sidebar(float x, float y, float w, float h) {
    const Palette& c = Colours();
    float header = 48 * scale_, panel = 52 * scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.sidebar);
    ImGui::BeginChild("side", ImVec2(w, h), 0, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    std::string title = guild_ && store_.guilds.count(guild_) ? store_.guilds[guild_].name : "Direct messages";
    ImGui::PushFont(fonts_.semibold, kBody);
    Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(o.x + 16 * scale_, o.y + (header - ImGui::GetFontSize()) / 2),
          U32(c.heading), title, w - 32 * scale_, [this](const std::string& u) { return texture(u); });
    ImGui::PopFont();
    dl->AddLine(ImVec2(o.x, o.y + header - 1), ImVec2(o.x + w, o.y + header - 1), U32(c.border), 1);

    ImGui::SetCursorPos(ImVec2(0, header));
    ImGui::BeginChild("list", ImVec2(w, h - header - panel), 0);
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
    float row = 32 * scale_, side = 8 * scale_;
    for (uint64_t id : store_.GuildChannels(guild_)) {
        const Channel& ch = store_.channels[id];
        if (ch.type == chan::Category) {
            ImGui::Dummy(ImVec2(1, 10 * scale_));
            std::string name = ch.name;
            std::transform(name.begin(), name.end(), name.begin(), ::toupper);
            ImGui::SetCursorPosX(side + 8 * scale_);
            ImGui::PushFont(fonts_.semibold, 12);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(1, 2 * scale_));
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
        if (selected || hov) dl->AddRectFilled(a, b, U32(selected ? c.selected : c.hover), 4 * scale_);
        if (unread && !selected)
            dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, a.y + row / 2 - 4 * scale_),
                              ImVec2(ImGui::GetWindowPos().x + 4 * scale_, a.y + row / 2 + 4 * scale_), U32(c.heading),
                              4 * scale_, ImDrawFlags_RoundCornersRight);
        ImU32 col = selected || unread ? U32(c.heading) : hov ? U32(c.text) : U32(c.muted, muted ? 0.5f : 1.0f);
        ImGui::PushFont(fonts_.regular, 20);
        float hashY = a.y + (row - ImGui::GetFontSize()) / 2;
        dl->AddText(ImVec2(a.x + 8 * scale_, hashY), U32(c.muted, muted ? 0.5f : 1.0f), ch.type == chan::News ? "\xE2\x80\xBC" : "#");
        ImGui::PopFont();
        ImGui::PushFont(unread || selected ? fonts_.semibold : fonts_.regular, kBody);
        float right = mentions ? 28 * scale_ : 4 * scale_;
        float avail = b.x - right - (a.x + 30 * scale_);
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 30 * scale_, a.y + (row - ImGui::GetFontSize()) / 2), col,
              ch.name, avail, images);
        ImGui::PopFont();
        if (mentions) {
            std::string n = std::to_string(mentions);
            ImGui::PushFont(fonts_.bold, 12);
            ImVec2 sz = ImGui::CalcTextSize(n.c_str());
            float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 16 * scale_;
            ImVec2 ba(b.x - bw - 8 * scale_, a.y + (row - bh) / 2);
            dl->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), U32(c.dnd), bh);
            dl->AddText(ImVec2(ba.x + (bw - sz.x) / 2, ba.y + (bh - sz.y) / 2), IM_COL32_WHITE, n.c_str());
            ImGui::PopFont();
        }
        if (hov && !ch.topic.empty()) ImGui::SetItemTooltip("%s", ch.topic.substr(0, 200).c_str());
        if (clicked) open(guild_, id);
        ImGui::Dummy(ImVec2(1, 2 * scale_));
    }
}

void App::dmList(float w) {
    ImageFor images = [this](const std::string& u) { return texture(u); };
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float row = 42 * scale_, side = 8 * scale_, av = 32 * scale_;
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
        if (selected || hov) dl->AddRectFilled(a, b, U32(selected ? c.selected : c.hover), 4 * scale_);
        ImVec2 ap(a.x + 8 * scale_, a.y + (row - av) / 2);
        if (ch.type == chan::DM && !ch.recipients.empty()) {
            avatar(ch.recipients[0], 0, ap, av, true);
        } else {
            ImTextureID t = ch.icon.empty() ? ImTextureID(0)
                                            : texture("https://cdn.discordapp.com/channel-icons/" + IdString(id) + "/" + ch.icon +
                                                      ".png?size=64");
            if (t)
                dl->AddImageRounded(ImTextureRef(t), ap, ImVec2(ap.x + av, ap.y + av), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, av / 2);
            else {
                dl->AddCircleFilled(ImVec2(ap.x + av / 2, ap.y + av / 2), av / 2, U32(c.accent));
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
        if (ch.type == chan::GroupDM) ty -= 7 * scale_;
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ty),
              selected || unread ? U32(c.heading) : U32(hov ? c.text : c.muted), name, avail, images);
        ImGui::PopFont();
        if (ch.type == chan::GroupDM) {
            ImGui::PushFont(fonts_.regular, 12);
            std::string n = std::to_string(ch.recipients.size() + 1) + " members";
            dl->AddText(ImVec2(tx, ty + 19 * scale_), U32(c.muted), n.c_str());
            ImGui::PopFont();
        }
        if (mentions) {
            std::string n = std::to_string(mentions);
            ImGui::PushFont(fonts_.bold, 12);
            ImVec2 sz = ImGui::CalcTextSize(n.c_str());
            float bw = std::max(sz.x + 8 * scale_, 16 * scale_), bh = 16 * scale_;
            ImVec2 ba(b.x - bw - 8 * scale_, a.y + (row - bh) / 2);
            dl->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), U32(c.dnd), bh);
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
        dl->AddImageRounded(ImTextureRef(t), at, ImVec2(at.x + size, at.y + size), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                            size / 2);
    else
        dl->AddCircleFilled(ImVec2(at.x + size / 2, at.y + size / 2), size / 2, U32(Colours().selected));
    if (!status) return;
    auto p = store_.presence.find(user);
    std::string s = p == store_.presence.end() ? "" : p->second;
    if (s.empty() || s == "offline" || s == "invisible") return;
    float r = size * 0.18f;
    ImVec2 centre(at.x + size - r, at.y + size - r);
    dl->AddCircleFilled(centre, r + 2 * scale_, U32(Colours().sidebar));
    dl->AddCircleFilled(centre, r, statusColour(s));
}

// --- member list ---

void App::memberList(float x, float y, float w, float h) {
    ImageFor images = [this](const std::string& u) { return texture(u); };
    const Palette& c = Colours();
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.sidebar);
    ImGui::BeginChild("members", ImVec2(w, h), 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float row = 42 * scale_, head = 36 * scale_, av = 32 * scale_, side = 8 * scale_;
    ImGui::Dummy(ImVec2(1, 8 * scale_));

    auto member = [&](uint64_t uid, const std::string& status) {
        ImGui::SetCursorPosX(side);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::PushID(int(uid % 0x7FFFFFFF));
        ImGui::InvisibleButton("m", ImVec2(w - side * 2, row));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(a, ImVec2(a.x + w - side * 2, a.y + row), U32(c.hover), 4 * scale_);
        bool offline = status.empty() || status == "offline";
        if (offline) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        avatar(uid, guild_, ImVec2(a.x + 8 * scale_, a.y + (row - av) / 2), av, true);
        std::string name = store_.DisplayName(guild_, uid);
        ImGui::PushFont(fonts_.semibold, kBody);
        float tx = a.x + av + 20 * scale_, avail = a.x + w - side * 2 - tx - 4 * scale_;
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
                    ImGui::SetCursorPos(ImVec2(side + 8 * scale_, yy + 16 * scale_));
                    ImGui::PushFont(fonts_.semibold, 12);
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
        if (ImGui::Checkbox("Notify me of mentions and DMs", &config_.notify)) SaveConfig(config_);
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
                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), U32(c.hover), 4 * scale_);
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
