// Drawing the open channel: its title bar header, messages with their replies,
// attachments, embeds and reactions, and the composer.
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>

#include "app.h"
#include "gif.h"

namespace disky {

namespace {

constexpr int64_t kGroupGap = 7 * 60 * 1000;  // messages closer than this share a header
constexpr float stickerSize = 128;
constexpr float kNames = 130;  // the name column's width, before scaling

bool normalType(int t) { return t == 0 || t == 19 || t == 20 || t == 21 || t == 23; }

std::string withParams(const std::string& url, const std::string& params) {
    return url + (url.find('?') == std::string::npos ? "?" : "&") + params;
}

// fit scales a size down to fit a box, keeping its shape.
ImVec2 fit(float w, float h, float maxW, float maxH) {
    if (w <= 0 || h <= 0) return ImVec2(std::min(maxW, 200.0f), std::min(maxH, 150.0f));
    float s = std::min({1.0f, maxW / w, maxH / h});
    return ImVec2(std::floor(w * s), std::floor(h * s));
}

std::string fileSize(uint64_t n) {
    char buf[32];
    if (n >= (1u << 20)) snprintf(buf, sizeof(buf), "%.1f MB", n / 1048576.0);
    else if (n >= 1024) snprintf(buf, sizeof(buf), "%.0f KB", n / 1024.0);
    else snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)n);
    return buf;
}

std::string clock(double s) {
    int t = int(s);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

bool sameDay(int64_t a, int64_t b) {
    auto day = [](int64_t ms) {
        ULARGE_INTEGER u;
        u.QuadPart = uint64_t(ms) * 10000 + 116444736000000000ull;
        FILETIME ft{u.LowPart, u.HighPart};
        SYSTEMTIME utc, local;
        FileTimeToSystemTime(&ft, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        return local.wYear * 400 + local.wMonth * 32 + local.wDay;
    };
    return day(a) == day(b);
}

std::string dayLabel(int64_t ms) {
    ULARGE_INTEGER u;
    u.QuadPart = uint64_t(ms) * 10000 + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, t;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t);
    static const char* months[] = {"January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"};
    char buf[48];
    snprintf(buf, sizeof(buf), "%d %s %d", t.wDay, months[t.wMonth - 1], t.wYear);
    return buf;
}

// iconButton is a flat button showing an icon glyph.
bool iconButton(const char* id, const char* glyph, ImVec2 size, float fontSize, ImFont* font, const char* tip = nullptr) {
    ImGui::PushID(id);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("b", size);
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& c = Colours();
    if (hov) dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), U32(c.hover));
    ImGui::PushFont(font, fontSize);
    ImVec2 sz = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(a.x + (size.x - sz.x) / 2, a.y + (size.y - sz.y) / 2), U32(hov ? c.heading : c.muted), glyph);
    ImGui::PopFont();
    if (tip && hov) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return pressed;
}

}  // namespace

const Layout& App::textLayout(const std::string& key, const std::string& text, float w, uint64_t guild, float size,
                              bool muted) {
    Cached& c = textLayouts_[key];
    size_t version = std::hash<std::string>()(text) ^ size_t(size * 7);
    if (c.width != w || c.version != version) {
        c.layout = LayOut(Parse(text), w, fonts_, resolver(guild), scale_, false, size, muted);
        c.width = w;
        c.version = version;
    }
    return c.layout;
}

// --- the channel ---

void App::chat(float x, float y, float w, float h) {
    if (!streamKey_.empty() && streamShown_) {
        streamView(x, y, w, h);
        return;
    }
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("chat", ImVec2(w, h), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!channel_) {
        const char* text = guild_ ? "No channels you can read here." : "Pick a conversation.";
        ImVec2 sz = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((w - sz.x) / 2, (h - sz.y) / 2));
        ImGui::TextDisabled("%s", text);
        ImGui::EndChild();
        return;
    }
    // The channel's name and topic are in the title bar (channelHeader),
    // with the member list button.
    float composerH = composerH_ > 0 ? composerH_ : 68 * scale_;
    ImGui::SetCursorPos(ImVec2(0, 0));
    messages(w, h - composerH);
    composer(0, h - composerH, w, &composerH_);
    ImGui::EndChild();
}

void App::channelHeader(float left, float right) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth(), barH = captionH_;
    const Channel& ch = store_.channels[channel_];
    std::string title = ch.IsPrivate() ? store_.ChannelName(channel_) : "#" + ch.name;
    std::string topic = ch.topic.substr(0, ch.topic.find('\n'));
    NoteText(topic);
    ImageFor images = [this](const std::string& u) { return texture(u); };

    // Centred in the window, kept clear of what's either side.
    ImGui::PushFont(fonts_.semibold, kSmall);
    float titleW = Label(nullptr, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0, 0), 0, title, 0, images);
    ImGui::PopFont();
    ImGui::PushFont(fonts_.regular, kSmall);
    float gap = 14 * scale_;
    float topicW = topic.empty() ? 0 : ImGui::CalcTextSize(topic.c_str()).x;
    float side = std::max(left, right) + 12 * scale_;
    float avail = w - side * 2;
    titleW = std::min(titleW, avail);
    float room = avail - titleW - gap;
    std::string shown = topic;
    if (topicW > room) {
        if (room < 60 * scale_) {
            shown.clear();
        } else {
            while (!shown.empty() && ImGui::CalcTextSize((shown + "…").c_str()).x > room) shown.pop_back();
            // Don't cut a UTF-8 character in half.
            while (!shown.empty() && (shown.back() & 0xC0) == 0x80) shown.pop_back();
            if (!shown.empty() && (unsigned char)shown.back() >= 0xC0) shown.pop_back();
            shown += "…";
        }
        topicW = shown.empty() ? 0 : ImGui::CalcTextSize(shown.c_str()).x;
    }
    float total = titleW + (shown.empty() ? 0 : gap + topicW);
    float x = o.x + (w - total) / 2, ty = o.y + (barH - ImGui::GetFontSize()) / 2;
    ImGui::PopFont();
    ImGui::PushFont(fonts_.semibold, kSmall);
    Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, ty), U32(c.heading), title, titleW, images);
    ImGui::PopFont();
    if (!shown.empty()) {
        float lx = x + titleW + gap / 2;
        dl->AddLine(ImVec2(lx, o.y + 9 * scale_), ImVec2(lx, o.y + barH - 9 * scale_), U32(c.border));
        ImGui::PushFont(fonts_.regular, kSmall);
        dl->AddText(ImVec2(x + titleW + gap, ty), U32(c.muted), shown.c_str());
        ImGui::PopFont();
        // The whole topic when it's cut short.
        if (shown != topic && ImGui::IsMouseHoveringRect(ImVec2(x + titleW + gap, o.y), ImVec2(x + total, o.y + barH)) &&
            ImGui::IsWindowHovered()) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(480 * scale_, FLT_MAX));
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(460 * scale_);
            ImGui::TextUnformatted(ch.topic.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

// --- messages ---

float App::messageHeight(const Message& m, bool groupStart, float w) {
    float contentW = w - (32 + kNames + 12) * scale_;
    float h = (groupStart ? 4 : 0) * scale_;
    if (m.reply || m.command) h += 20 * scale_;
    float line = kBody * 1.3f * scale_;
    if (!normalType(m.type)) return h + line + 2 * scale_;
    float body = 0;
    if (!m.content.empty()) body += layoutFor(m, contentW).height;
    body += attachmentsHeight(m, contentW);
    for (const auto& e : m.embeds) body += embedHeight(e, contentW) + 4 * scale_;
    body += m.stickers.size() * (stickerSize + 4) * scale_;
    body += reactionsHeight(m, contentW);
    if (m.failed) body += 20 * scale_;
    // At least a line, for the name beside it.
    return h + std::max(body, line) + 2 * scale_;
}

void App::messages(float w, float h) {
    const Palette& c = Colours();
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10 * scale_);
    // The scrollbar's always there, so the width is known before laying
    // out, and the scroll can be set before the list is drawn: set after,
    // it would take a frame, and a new message would show below the view.
    float cw = w - 10 * scale_;
    Messages& ms = store_.messages[channel_];
    if (!ms.loaded) {
        ImGui::BeginChild("messages", ImVec2(w, h), 0);
        const char* t = "Loading…";
        ImVec2 sz = ImGui::CalcTextSize(t);
        ImGui::SetCursorPos(ImVec2((cw - sz.x) / 2, (h - sz.y) / 2));
        ImGui::TextDisabled("%s", t);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    // Lay everything out: each message's height, with day lines between.
    struct Row {
        const Message* m;
        bool groupStart, alt;
        float y, h;
        bool day;
    };
    std::vector<Row> rows;
    rows.reserve(ms.list.size());
    float top = ms.reachedStart ? 96 * scale_ : 48 * scale_;
    float y = top;
    bool alt = true;
    const Message* prev = nullptr;
    for (const auto& m : ms.list) {
        bool day = !prev || !sameDay(prev->timestamp, m.timestamp);
        bool gs = !prev || day || prev->authorId != m.authorId || m.timestamp - prev->timestamp > kGroupGap ||
                  m.reply.has_value() || m.command.has_value() || !normalType(m.type) || !normalType(prev->type);
        if (day) y += 24 * scale_;
        // Each author's run of messages takes the other background from
        // the one before, so it's easy to see where one ends.
        if (gs) alt = !alt;
        float mh = messageHeight(m, gs, cw);
        rows.push_back({&m, gs, alt, y, mh, day});
        y += mh;
        prev = &m;
    }
    float total = y + 16 * scale_;
    float maxY = std::max(0.0f, total - h);

    // Keep the view still when older messages arrive above it, and at the
    // bottom while the reader's there (until they turn the wheel up).
    ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::GetIO().MouseWheel > 0 && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + w, at.y + h)) && maxY > 0)
        atBottom_ = false;
    if (atBottom_) {
        ImGui::SetNextWindowScroll(ImVec2(-1, maxY));
    } else if (firstShown_ && !rows.empty() && rows.front().m->id != firstShown_) {
        for (const auto& r : rows)
            if (r.m->id == firstShown_) {
                ImGui::SetNextWindowScroll(ImVec2(-1, scrollY_ + r.y - firstShownY_));
                break;
            }
    }
    if (!rows.empty()) {
        firstShown_ = rows.front().m->id;
        firstShownY_ = rows.front().y;
    }
    // The height's known now, so tell ImGui: otherwise it limits the
    // scroll to last frame's height, and a new message spends a frame
    // below the view. (The +1 is the Dummy that ends the list.)
    ImGui::SetNextWindowContentSize(ImVec2(0, total + 1));
    ImGui::BeginChild("messages", ImVec2(w, h), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float scroll = ImGui::GetScrollY();
    float view = ImGui::GetWindowHeight();
    hovered_ = 0;
    if (ms.reachedStart) {
        ImGui::SetCursorPos(ImVec2(16 * scale_, 24 * scale_));
        ImGui::PushFont(fonts_.bold, kH1);
        ImGui::PushStyleColor(ImGuiCol_Text, c.heading);
        const Channel& ch = store_.channels[channel_];
        ImGui::Text("%s%s", ch.IsPrivate() ? "" : "Welcome to #", ch.IsPrivate() ? store_.ChannelName(channel_).c_str() : ch.name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorPosX(16 * scale_);
        ImGui::TextDisabled(ch.IsPrivate() ? "This is the beginning of your conversation." : "This is the start of the channel.");
    } else {
        ImGui::SetCursorPos(ImVec2(16 * scale_, 16 * scale_));
        ImGui::TextDisabled("Loading older messages…");
    }
    for (const auto& r : rows) {
        float above = r.day ? 24 * scale_ : 0;
        if (r.y + r.h < scroll - 200 * scale_ || r.y - above > scroll + view + 200 * scale_) continue;
        if (r.day) {
            ImVec2 o = ImGui::GetWindowPos();
            float ly = o.y + r.y - above / 2 - scroll;
            ImU32 col = U32(c.border);
            dl->AddLine(ImVec2(o.x + 16 * scale_, ly), ImVec2(o.x + cw - 16 * scale_, ly), col);
            ImGui::PushFont(fonts_.semibold, 12);
            if (r.day) {
                std::string label = dayLabel(r.m->timestamp);
                ImVec2 sz = ImGui::CalcTextSize(label.c_str());
                ImVec2 la(o.x + (cw - sz.x) / 2, ly - sz.y / 2);
                dl->AddRectFilled(ImVec2(la.x - 6 * scale_, la.y), ImVec2(la.x + sz.x + 6 * scale_, la.y + sz.y), U32(c.bg));
                dl->AddText(la, U32(c.muted), label.c_str());
            }
            ImGui::PopFont();
        }
        ImGui::SetCursorPos(ImVec2(0, r.y));
        message(*r.m, r.groupStart, r.alt, cw, r.h);
    }
    ImGui::SetCursorPos(ImVec2(0, total));
    ImGui::Dummy(ImVec2(1, 1));

    // Scrolling: stay at the bottom unless the reader has scrolled up.
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    bool userScrolled = (ImGui::IsWindowHovered() && io.MouseWheel != 0) ||
                        ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(win, ImGuiAxis_Y) ||
                        (ImGui::IsWindowFocused() && (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_PageDown)));
    if (userScrolled) atBottom_ = scroll >= maxY - 4 * scale_ && io.MouseWheel <= 0;
    else if (!atBottom_ && scroll >= maxY - 2 && maxY > 0) atBottom_ = true;
    scrollY_ = scroll;
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(scroll - view * 0.8f);
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(scroll + view * 0.8f);
    if (scroll < 600 * scale_ && !ms.reachedStart && !ms.loadingOlder && (!atBottom_ || total < view + 600 * scale_))
        loadOlder(channel_);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Far up the history: a way back.
    if (!atBottom_ && scroll < maxY - view * 1.5f) {
        ImVec2 o = ImGui::GetItemRectMin();
        float bw = 170 * scale_, bh = 28 * scale_;
        ImGui::SetCursorScreenPos(ImVec2(o.x + (w - bw) / 2, o.y + h - bh - 10 * scale_));
        ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.accent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh / 2);
        if (ImGui::Button("Jump to present", ImVec2(bw, bh))) atBottom_ = true;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}

void App::message(const Message& m, bool groupStart, bool alt, float w, float h) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    // Names in a column on the left, right-aligned; messages beside them.
    float nameW = kNames * scale_;
    float left = (16 + kNames + 12) * scale_, contentW = w - left - 16 * scale_;
    float y = o.y + (groupStart ? 4 : 0) * scale_;
    float rowTop = o.y;
    if (alt) dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), U32(c.band));
    ImVec2 mouse = ImGui::GetMousePos();
    bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mouse.y >= rowTop &&
               mouse.y < o.y + h && mouse.x >= o.x && mouse.x < o.x + w && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    // Messages that would notify get a faint line down the left; not in DMs,
    // where every message does.
    auto ch = store_.channels.find(m.channelId);
    bool dm = ch != store_.channels.end() && ch->second.IsPrivate();
    bool mentioned = !dm && (store_.MentionsMe(m) || (m.reply && m.reply->authorId == store_.me && m.authorId != store_.me));
    if (hov || contextMessage_ == m.id) {
        hovered_ = m.id;
        dl->AddRectFilled(ImVec2(o.x, rowTop), ImVec2(o.x + w, o.y + h), U32(c.hover));
    }
    if (mentioned) dl->AddRectFilled(ImVec2(o.x, rowTop), ImVec2(o.x + 2 * scale_, o.y + h), U32(c.mentionBar));
    std::string author = store_.DisplayName(m.guildId, m.authorId);

    // Who ran the command this answers, in a line above.
    if (m.command && !m.reply) {
        ImGui::PushFont(fonts_.regular, 13);
        float rx = o.x + left, ry = y + 2 * scale_;
        dl->AddText(ImVec2(rx, ry), U32(c.muted), "\xE2\x86\xB1");  // ↱
        rx += 16 * scale_;
        std::string name = "@" + store_.DisplayName(m.guildId, m.command->userId);
        ImGui::PushFont(fonts_.semibold, 13);
        rx += Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(rx, ry), userColour(m.guildId, m.command->userId), name, 0,
                    [this](const std::string& u) { return texture(u); }) +
              6 * scale_;
        ImGui::PopFont();
        dl->AddText(ImVec2(rx, ry), U32(c.muted), "used");
        rx += ImGui::CalcTextSize("used").x + 6 * scale_;
        // Slash commands get their slash; context menu ones (type 23) don't.
        std::string cmd = (m.type == 23 ? "" : "/") + m.command->name;
        if (!m.command->name.empty()) {
            dl->AddText(ImVec2(rx, ry), U32(c.link), cmd.c_str());
            rx += ImGui::CalcTextSize(cmd.c_str()).x + 6 * scale_;
        }
        if (m.flags & 64) dl->AddText(ImVec2(rx, ry), U32(c.muted), "·  only you can see this");
        ImGui::PopFont();
        y += 20 * scale_;
    }

    // The reply's original, in a line above.
    if (m.reply) {
        ImGui::PushFont(fonts_.regular, 13);
        float rx = o.x + left, ry = y + 2 * scale_;
        dl->AddText(ImVec2(rx, ry), U32(c.muted), "\xE2\x86\xB1");  // ↱
        rx += 16 * scale_;
        if (m.reply->deleted) {
            dl->AddText(ImVec2(rx, ry), U32(c.muted), "Original message was deleted");
        } else {
            std::string name = "@" + store_.DisplayName(m.guildId, m.reply->authorId);
            ImGui::PushFont(fonts_.semibold, 13);
            rx += Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(rx, ry), userColour(m.guildId, m.reply->authorId), name, 0,
                        [this](const std::string& u) { return texture(u); }) +
                  6 * scale_;
            ImGui::PopFont();
            std::string text = PlainText(m.reply->content, resolver(m.guildId));
            if (text.empty()) text = m.reply->attachments ? "Click to see attachment" : "";
            Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(rx, ry), U32(c.muted), text, o.x + w - 16 * scale_ - rx,
                  [this](const std::string& u) { return texture(u); });
        }
        ImGui::PopFont();
        y += 20 * scale_;
    }

    // The author, at the start of their run of messages.
    float textY = y + (kBody * 1.3f * scale_ - kBody * scale_) / 2;
    if (groupStart || !normalType(m.type)) {
        ImGui::PushFont(fonts_.semibold, kBody);
        bool bot = false;
        if (const User* u = store_.FindUser(m.authorId)) bot = u->bot;
        float badge = 0;
        if (bot) {
            ImGui::PushFont(fonts_.semibold, 10);
            badge = ImGui::CalcTextSize("APP").x + 12 * scale_;
            ImGui::PopFont();
        }
        ImageFor images = [this](const std::string& u) { return texture(u); };
        float nw = Label(nullptr, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(), 0, author, nameW - badge, images);
        float nx = o.x + 16 * scale_ + nameW - nw;
        Label(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(nx, textY), userColour(m.guildId, m.authorId), author,
              nameW - badge, images);
        ImGui::PopFont();
        if (bot) {
            ImGui::PushFont(fonts_.semibold, 10);
            ImVec2 sz = ImGui::CalcTextSize("APP");
            float bx = nx - badge + 2 * scale_, by = textY + 3 * scale_;
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + sz.x + 6 * scale_, by + sz.y + 2 * scale_), U32(c.accent));
            dl->AddText(ImVec2(bx + 3 * scale_, by + 1 * scale_), IM_COL32_WHITE, "APP");
            ImGui::PopFont();
        }
        if (ImGui::IsMouseHoveringRect(ImVec2(nx, textY), ImVec2(o.x + 16 * scale_ + nameW, textY + kBody * scale_)) && hov)
            if (const User* u = store_.FindUser(m.authorId)) ImGui::SetTooltip("%s", u->username.c_str());
    }
    if (hov) {
        // The time, on the right while hovered, clear of the toolbar.
        ImGui::PushFont(fonts_.regular, 11);
        std::string t = formatTime(m.timestamp, true);
        float tw = ImGui::CalcTextSize(t.c_str()).x;
        dl->AddText(ImVec2(o.x + w - 120 * scale_ - tw, textY + 2 * scale_), U32(c.muted), t.c_str());
        ImGui::PopFont();
    }

    if (!normalType(m.type)) {
        static const std::map<int, const char*> lines = {
            {1, "added someone to the group."},  {2, "left the group."},
            {3, "started a call."},              {4, "changed the channel name."},
            {5, "changed the channel icon."},    {6, "pinned a message to this channel."},
            {7, "joined the server."},           {8, "boosted the server!"},
            {9, "boosted the server to level 1!"}, {10, "boosted the server to level 2!"},
            {11, "boosted the server to level 3!"}, {12, "followed a channel here."},
            {18, "started a thread."},
        };
        auto it = lines.find(m.type);
        std::string line = it != lines.end() ? it->second : (m.content.empty() ? "did something Discord didn't explain." : m.content);
        ImGui::PushFont(fonts_.italic, kBody);
        dl->AddText(ImVec2(o.x + left, textY), U32(c.muted), line.c_str());
        ImGui::PopFont();
        return;
    }

    // A command's answer the app's still working on.
    if (m.content.empty() && m.embeds.empty() && (m.flags & 128)) {
        ImGui::PushFont(fonts_.italic, kBody);
        dl->AddText(ImVec2(o.x + left, textY), U32(c.muted), (author + " is thinking…").c_str());
        ImGui::PopFont();
    }

    // Content.
    if (!m.content.empty()) {
        const Layout& l = layoutFor(m, contentW);
        auto& revealed = revealed_[m.id];
        // Animated emoji play while the message is hovered, as in Discord.
        Hit hit = Draw(l, ImVec2(o.x + left, y), [this, hov](const std::string& url) { return image(url, hov && IsGifUrl(url)); },
                       revealed);
        if (m.edited && !l.pieces.empty()) {
            const Piece& last = l.pieces.back();
            ImGui::PushFont(fonts_.regular, 11);
            dl->AddText(ImVec2(o.x + left + last.pos.x + last.size.x + 4 * scale_, y + last.pos.y + last.size.y - ImGui::GetFontSize() - 2 * scale_),
                        U32(c.muted), "(edited)");
            ImGui::PopFont();
        }
        if (hit.piece) {
            const Piece& p = *hit.piece;
            bool hidden = p.spoiler && std::find(revealed.begin(), revealed.end(), p.spoiler) == revealed.end();
            if (hidden || !p.link.empty() || p.channel) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (!p.link.empty() && !hidden) ImGui::SetTooltip("%s", p.link.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (hidden)
                    revealed.push_back(p.spoiler);
                else if (!p.link.empty())
                    OpenURL(p.link);
                else if (p.channel && store_.channels.count(p.channel) && store_.CanView(p.channel))
                    open(store_.channels[p.channel].guildId, p.channel);
            }
        }
        y += l.height;
    }
    attachments(m, o.x + left, y, contentW);
    y += attachmentsHeight(m, contentW);
    for (size_t i = 0; i < m.embeds.size(); i++) {
        embed(m, m.embeds[i], o.x + left, y + 2 * scale_, contentW);
        y += embedHeight(m.embeds[i], contentW) + 4 * scale_;
    }
    for (const auto& st : m.stickers) {
        // Lottie stickers are vector animations: just the name for those.
        float sz = stickerSize * scale_;
        ImVec2 sa(o.x + left, y + 2 * scale_);
        bool overSticker = hov && ImGui::IsMouseHoveringRect(sa, ImVec2(sa.x + sz, sa.y + sz));
        ImTextureID t = st.format == 3 ? ImTextureID(0)
                                       : image("https://media.discordapp.net/stickers/" + IdString(st.id) +
                                                   (st.format == 4 ? ".gif" : ".png") + "?size=160",
                                               overSticker && st.format == 4);
        if (t)
            dl->AddImage(ImTextureRef(t), sa, ImVec2(sa.x + sz, sa.y + sz));
        else
            dl->AddText(ImVec2(sa.x, sa.y), U32(c.muted), ("Sticker: " + st.name).c_str());
        if (overSticker) ImGui::SetTooltip("%s", st.name.c_str());
        y += sz + 4 * scale_;
    }
    reactions(m, o.x + left, y, contentW);
    y += reactionsHeight(m, contentW);
    if (m.failed) {
        ImGui::PushFont(fonts_.regular, 13);
        dl->AddText(ImVec2(o.x + left, y + 2 * scale_), U32(c.bad), "Not sent.");
        ImGui::SetCursorScreenPos(ImVec2(o.x + left + 70 * scale_, y));
        ImGui::PushID(int(std::hash<std::string>()(m.nonce) & 0x7FFFFFFF));
        if (ImGui::SmallButton("Delete")) deleteMessage(m.channelId, m.id);
        ImGui::PopID();
        ImGui::PopFont();
    }

    if (hov && !m.pending) toolbar(m, ImVec2(o.x + w - 16 * scale_, rowTop));

    // Right-click: everything the toolbar has, and copying.
    std::string menuId = "msg" + IdString(m.id);
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !m.pending) {
        ImGui::OpenPopup(menuId.c_str());
        contextMessage_ = m.id;
    }
    if (ImGui::BeginPopup(menuId.c_str())) {
        bool mine = m.authorId == store_.me;
        bool canManage = mine || (perms(m.channelId) & perm::ManageMessages);
        if (ImGui::MenuItem("Add reaction")) {
            pickerMessage_ = m.id;
            pickerAt_ = ImGui::GetMousePos();
            pickerOpen_ = true;
        }
        if (canSend(m.channelId) && ImGui::MenuItem("Reply")) {
            replyTo_ = m.id;
            editing_ = 0;
            focusComposer_ = true;
        }
        if (mine && ImGui::MenuItem("Edit")) {
            drafts_[channel_] = draft_.data();
            std::fill(draft_.begin(), draft_.end(), 0);
            strncpy(draft_.data(), m.content.c_str(), draft_.size() - 1);
            editing_ = m.id;
            replyTo_ = 0;
            focusComposer_ = true;
        }
        if (!m.content.empty() && ImGui::MenuItem("Copy text")) ImGui::SetClipboardText(m.content.c_str());
        std::string link = "https://discord.com/channels/" + (m.guildId ? IdString(m.guildId) : std::string("@me")) + "/" +
                           IdString(m.channelId) + "/" + IdString(m.id);
        if (ImGui::MenuItem("Copy message link")) ImGui::SetClipboardText(link.c_str());
        if (canManage) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, c.bad);
            if (ImGui::MenuItem("Delete message")) confirmDelete_ = m.id;
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    } else if (contextMessage_ == m.id) {
        contextMessage_ = 0;
    }

    if (confirmDelete_ == m.id) {
        ImGui::OpenPopup("Delete message?");
        confirmDelete_ = 0;
        contextMessage_ = m.id;
    }
    // Only size the dialog when it's this message's: a size set and not
    // used carries over to the next window, such as a tooltip.
    if (contextMessage_ == m.id) ImGui::SetNextWindowSize(ImVec2(380 * scale_, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * scale_, 16 * scale_));
    if (contextMessage_ == m.id && ImGui::BeginPopupModal("Delete message?", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("Delete this message? This can't be undone.");
        ImGui::TextDisabled("Hold Shift when clicking the bin to skip this.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, c.bad);
        if (ImGui::Button("Delete", ImVec2(100 * scale_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            deleteMessage(m.channelId, m.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100 * scale_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void App::toolbar(const Message& m, ImVec2 topRight) {
    const Palette& c = Colours();
    bool mine = m.authorId == store_.me;
    bool canManage = mine || (perms(m.channelId) & perm::ManageMessages);
    bool canReact = !guild_ || (perms(m.channelId) & perm::AddReactions);
    int n = (canReact ? 1 : 0) + (canSend(m.channelId) ? 1 : 0) + (mine ? 1 : 0) + (canManage ? 1 : 0);
    if (n == 0) return;
    float b = 22 * scale_, pad = 1 * scale_;
    float w = n * b + pad * 2;
    ImVec2 a(topRight.x - w, std::max(topRight.y - 10 * scale_, ImGui::GetWindowPos().y + 2 * scale_));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + b + pad * 2), U32(c.bg));
    dl->AddRect(a, ImVec2(a.x + w, a.y + b + pad * 2), U32(c.border));
    float x = a.x + pad;
    auto button = [&](const char* id, const char* glyph, const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(x, a.y + pad));
        x += b;
        return iconButton(id, glyph, ImVec2(b, b), 12, fonts_.regular, tip);
    };
    ImGui::PushID(int(m.id & 0x7FFFFFFF));
    if (canReact && button("react", icon::Emoji, "Add reaction")) {
        pickerMessage_ = m.id;
        pickerAt_ = ImVec2(x, a.y);
        pickerOpen_ = true;
    }
    if (canSend(m.channelId) && button("reply", icon::Reply, "Reply")) {
        replyTo_ = m.id;
        editing_ = 0;
        focusComposer_ = true;
    }
    if (mine && button("edit", icon::Edit, "Edit")) {
        drafts_[channel_] = draft_.data();
        std::fill(draft_.begin(), draft_.end(), 0);
        strncpy(draft_.data(), m.content.c_str(), draft_.size() - 1);
        editing_ = m.id;
        replyTo_ = 0;
        focusComposer_ = true;
    }
    if (canManage && button("delete", icon::Delete, "Delete (Shift to skip asking)")) {
        if (ImGui::GetIO().KeyShift)
            deleteMessage(m.channelId, m.id);
        else
            confirmDelete_ = m.id;
    }
    ImGui::PopID();
}

// --- attachments, embeds, media ---

float App::attachmentsHeight(const Message& m, float w) {
    float h = 0;
    for (const auto& a : m.attachments) {
        if (a.IsImage() || a.IsVideo())
            h += fit(float(a.width), float(a.height), std::min(w, 400 * scale_), 300 * scale_).y + 4 * scale_;
        else
            h += 60 * scale_;
    }
    return h;
}

void App::attachments(const Message& m, float x, float y, float w) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& a : m.attachments) {
        if (a.IsImage() || a.IsVideo()) {
            ImVec2 size = fit(float(a.width), float(a.height), std::min(w, 400 * scale_), 300 * scale_);
            std::string base = a.proxyUrl.empty() ? a.url : a.proxyUrl;
            int pw = int(size.x), ph = int(size.y);
            std::string dims = "width=" + std::to_string(pw) + "&height=" + std::to_string(ph);
            std::string still = base.empty() ? "" : a.IsVideo() ? withParams(base, "format=jpeg&" + dims) : withParams(base, dims);
            bool gif = a.contentType == "image/gif" || IsGifUrl(a.filename);
            media("a" + IdString(a.id), still, a.IsVideo() ? a.url : "", a.url, ImVec2(x, y + 2 * scale_), size, a.spoiler, gif);
            y += size.y + 4 * scale_;
        } else {
            float bw = std::min(w, 400 * scale_), bh = 52 * scale_;
            ImVec2 b0(x, y + 4 * scale_), b1(x + bw, y + 4 * scale_ + bh);
            dl->AddRectFilled(b0, b1, U32(c.code));
            dl->AddRect(b0, b1, U32(c.border));
            ImGui::SetCursorScreenPos(b0);
            ImGui::PushID(int(a.id & 0x7FFFFFFF));
            bool clicked = ImGui::InvisibleButton("file", ImVec2(bw, bh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImGui::PushFont(fonts_.regular, 26);
            dl->AddText(ImVec2(b0.x + 12 * scale_, b0.y + (bh - ImGui::GetFontSize()) / 2), U32(c.muted), icon::Attach);
            ImGui::PopFont();
            dl->AddText(ImVec2(b0.x + 48 * scale_, b0.y + 7 * scale_), U32(m.pending ? c.muted : c.link), a.filename.c_str());
            ImGui::PushFont(fonts_.regular, 12);
            dl->AddText(ImVec2(b0.x + 48 * scale_, b0.y + 29 * scale_), U32(c.muted), fileSize(a.size).c_str());
            ImGui::PopFont();
            if (hov && !a.url.empty()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open in your browser");
            }
            if (clicked && !a.url.empty()) OpenURL(a.url);
            y += 60 * scale_;
        }
    }
}

void App::media(const std::string& key, const std::string& still, const std::string& play, const std::string& link,
                ImVec2 at, ImVec2 size, bool spoiler, bool gif) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(at.x + size.x, at.y + size.y);
    bool gifVideo = gif && !play.empty();
    bool playing = !gifVideo && !play.empty() && video_->Url() == play;
    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(key.c_str());
    bool clicked = ImGui::InvisibleButton("media", size);
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    bool hidden = spoiler && !revealedMedia_.count(key);
    // GIFs play only while the mouse is over them.
    bool animate = gif && hov && !hidden;
    if (animate && gifVideo) {
        gifWanted_ = true;
        if (gif_->Url() != play)
            gif_->Play(play, 0, true);
        else if (gif_->Paused())
            gif_->TogglePause();
    }
    ID3D11ShaderResourceView* frame = playing                                        ? video_->Frame()
                                      : gifVideo && !hidden && gif_->Url() == play ? gif_->Frame()
                                                                                   : nullptr;
    if (frame) {
        dl->AddImage(ImTextureRef(ImTextureID((intptr_t)frame)), at, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
    } else if (ImTextureID t = still.empty() ? ImTextureID(0) : image(still, animate && !gifVideo)) {
        dl->AddImage(ImTextureRef(t), at, end, ImVec2(0, 0), ImVec2(1, 1), hidden ? IM_COL32(40, 40, 40, 255) : IM_COL32_WHITE);
    } else {
        dl->AddRectFilled(at, end, U32(c.code));
    }
    if (hidden) {
        dl->AddRectFilled(at, end, U32(c.spoiler, 0.85f));
        ImGui::PushFont(fonts_.bold, 13);
        ImVec2 sz = ImGui::CalcTextSize("SPOILER");
        ImVec2 p(at.x + (size.x - sz.x) / 2, at.y + (size.y - sz.y) / 2);
        dl->AddRectFilled(ImVec2(p.x - 10 * scale_, p.y - 5 * scale_), ImVec2(p.x + sz.x + 10 * scale_, p.y + sz.y + 5 * scale_), IM_COL32(0, 0, 0, 180));
        dl->AddText(p, IM_COL32_WHITE, "SPOILER");
        ImGui::PopFont();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) revealedMedia_.insert(key);
        return;
    }
    if (gif) {
        // A GIF badge in the corner while it's still, as Discord has.
        if (!hov) {
            ImGui::PushFont(fonts_.bold, 11);
            ImVec2 sz = ImGui::CalcTextSize("GIF");
            ImVec2 p(at.x + 6 * scale_, at.y + 6 * scale_);
            dl->AddRectFilled(p, ImVec2(p.x + sz.x + 8 * scale_, p.y + sz.y + 4 * scale_), IM_COL32(0, 0, 0, 170));
            dl->AddText(ImVec2(p.x + 4 * scale_, p.y + 2 * scale_), IM_COL32_WHITE, "GIF");
            ImGui::PopFont();
        }
        if (!link.empty()) {
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (clicked) OpenURL(link);
        }
        return;
    }
    if (playing) {
        std::string err = video_->Error();
        if (!err.empty()) {
            dl->AddText(ImVec2(at.x + 10 * scale_, at.y + 10 * scale_), U32(c.bad), err.c_str());
            if (clicked) OpenURL(link);
            return;
        }
        // Controls along the bottom while hovered or paused.
        float bar = 32 * scale_;
        bool showBar = (hov || video_->Paused() || video_->Ended()) && !video_->Looping();
        ImVec2 mouse = ImGui::GetMousePos();
        bool onBar = hov && mouse.y > end.y - bar;
        if (showBar) {
            dl->AddRectFilledMultiColor(ImVec2(at.x, end.y - bar * 1.5f), end, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                        IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200));
            float bx = at.x + 8 * scale_, by = end.y - bar + 4 * scale_;
            ImGui::PushFont(fonts_.regular, 15);
            const char* g = video_->Paused() || video_->Ended() ? icon::Play : icon::Pause;
            dl->AddText(ImVec2(bx, by + 2 * scale_), IM_COL32_WHITE, g);
            ImGui::PopFont();
            double pos = video_->Position(), dur = video_->Duration();
            std::string t = clock(pos) + " / " + clock(dur);
            ImGui::PushFont(fonts_.regular, 12);
            float tw = ImGui::CalcTextSize(t.c_str()).x;
            float trackX = bx + 28 * scale_, trackEnd = end.x - tw - 48 * scale_;
            dl->AddText(ImVec2(trackEnd + 8 * scale_, by + 4 * scale_), IM_COL32_WHITE, t.c_str());
            ImGui::PopFont();
            ImGui::PushFont(fonts_.regular, 14);
            dl->AddText(ImVec2(end.x - 28 * scale_, by + 3 * scale_), IM_COL32_WHITE, icon::Open);
            ImGui::PopFont();
            float ty = by + 11 * scale_;
            dl->AddRectFilled(ImVec2(trackX, ty - 2 * scale_), ImVec2(trackEnd, ty + 2 * scale_), IM_COL32(255, 255, 255, 80));
            if (dur > 0)
                dl->AddRectFilled(ImVec2(trackX, ty - 2 * scale_), ImVec2(trackX + float((trackEnd - trackX) * pos / dur), ty + 2 * scale_),
                                  U32(c.accent));
            if (clicked && onBar) {
                if (mouse.x < trackX)
                    video_->TogglePause();
                else if (mouse.x >= trackX && mouse.x <= trackEnd && dur > 0)
                    video_->Seek(dur * (mouse.x - trackX) / (trackEnd - trackX));
                else if (mouse.x > end.x - 32 * scale_) {
                    video_->Stop();
                    OpenURL(link);
                }
                return;
            }
            // Volume with the wheel while over the video.
        }
        if (hov && ImGui::GetIO().MouseWheel != 0 && !video_->Looping()) {
            config_.volume = std::clamp(config_.volume + ImGui::GetIO().MouseWheel * 0.05f, 0.0f, 1.0f);
            video_->SetVolume(config_.volume);
            ImGui::SetTooltip("Volume %d%%", int(config_.volume * 100 + 0.5f));
        }
        if (clicked) video_->TogglePause();
        return;
    }
    if (!play.empty() || !link.empty()) {
        if (!play.empty()) {
            float pr = 24 * scale_;
            ImVec2 mid((at.x + end.x) / 2, (at.y + end.y) / 2);
            dl->AddCircleFilled(mid, pr, IM_COL32(0, 0, 0, hov ? 220 : 160));
            ImGui::PushFont(fonts_.regular, 20);
            ImVec2 sz = ImGui::CalcTextSize(icon::Play);
            dl->AddText(ImVec2(mid.x - sz.x / 2 + 2 * scale_, mid.y - sz.y / 2), IM_COL32_WHITE, icon::Play);
            ImGui::PopFont();
        }
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) {
            if (!play.empty())
                video_->Play(play, config_.volume);
            else
                OpenURL(link);
        }
    }
}

float App::embedHeight(const Embed& e, float w) {
    if (e.type == "image" || e.type == "gifv") {
        const Media& m = e.type == "gifv" || e.thumbnail.width == 0 ? (e.thumbnail.width ? e.thumbnail : e.image) : e.thumbnail;
        return fit(float(m.width), float(m.height), std::min(w, 400 * scale_), 300 * scale_).y;
    }
    float bw = std::min(w, 432 * scale_), pad = 12 * scale_;
    bool thumb = !e.thumbnail.proxyUrl.empty() && e.image.proxyUrl.empty() && e.video.url.empty();
    float inner = bw - pad * 2 - 4 * scale_ - (thumb ? 88 * scale_ : 0);
    float h = pad;
    if (!e.provider.empty()) h += 18 * scale_;
    if (!e.author.empty()) h += 22 * scale_;
    if (!e.title.empty()) h += textLayout("t" + e.url + e.title, "**" + e.title + "**", inner, 0, 15, false).height + 4 * scale_;
    if (!e.description.empty()) h += textLayout("d" + e.url + std::to_string(std::hash<std::string>()(e.description)), e.description, inner, 0, 14, false).height + 4 * scale_;
    for (const auto& f : e.fields)
        h += 20 * scale_ + textLayout("f" + f.name + f.value, f.value, inner, 0, 14, false).height + 4 * scale_;
    const Media& big = !e.video.url.empty() ? (e.thumbnail.width ? e.thumbnail : e.video) : e.image;
    if (!e.image.proxyUrl.empty() || !e.video.url.empty())
        h += fit(float(big.width), float(big.height), bw - pad * 2 - 4 * scale_, 300 * scale_).y + 8 * scale_;
    if (thumb) h = std::max(h, pad + 80 * scale_);
    if (!e.footer.empty()) h += 20 * scale_;
    return h + pad;
}

void App::embed(const Message& m, const Embed& e, float x, float y, float w) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string key = IdString(m.id) + e.url + e.type;
    if (e.type == "image" || e.type == "gifv") {
        const Media& md = e.thumbnail.width ? e.thumbnail : e.image;
        ImVec2 size = fit(float(md.width), float(md.height), std::min(w, 400 * scale_), 300 * scale_);
        std::string still = md.proxyUrl.empty() ? md.url : md.proxyUrl;
        std::string play = e.type == "gifv" ? (e.video.proxyUrl.empty() ? e.video.url : e.video.proxyUrl) : "";
        media(key, still, play, e.url, ImVec2(x, y), size, false, e.type == "gifv" || IsGifUrl(still));
        return;
    }
    float bw = std::min(w, 432 * scale_), pad = 12 * scale_, h = embedHeight(e, w);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + h), U32(c.code));
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 4 * scale_, y + h), e.hasColour ? U32(Hex(e.colour)) : U32(c.border));
    bool thumb = !e.thumbnail.proxyUrl.empty() && e.image.proxyUrl.empty() && e.video.url.empty();
    float inner = bw - pad * 2 - 4 * scale_ - (thumb ? 88 * scale_ : 0);
    float ix = x + 4 * scale_ + pad, iy = y + pad;
    if (!e.provider.empty()) {
        ImGui::PushFont(fonts_.regular, 12);
        dl->AddText(ImVec2(ix, iy), U32(c.muted), e.provider.c_str());
        ImGui::PopFont();
        iy += 18 * scale_;
    }
    if (!e.author.empty()) {
        ImGui::PushFont(fonts_.semibold, 14);
        dl->AddText(ImVec2(ix, iy), U32(c.heading), e.author.c_str());
        ImGui::PopFont();
        iy += 22 * scale_;
    }
    auto text = [&](const Layout& l) {
        Hit hit = Draw(l, ImVec2(ix, iy), [this](const std::string& url) { return texture(url); }, {});
        if (hit.piece && !hit.piece->link.empty()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) OpenURL(hit.piece->link);
        }
        iy += l.height + 4 * scale_;
    };
    if (!e.title.empty()) {
        // A title with a URL is a link.
        std::string t = e.url.empty() ? "**" + e.title + "**" : "**[" + e.title + "](" + e.url + ")**";
        text(textLayout("t" + e.url + e.title, t, inner, 0, 15, false));
    }
    if (!e.description.empty())
        text(textLayout("d" + e.url + std::to_string(std::hash<std::string>()(e.description)), e.description, inner, m.guildId, 14, false));
    for (const auto& f : e.fields) {
        ImGui::PushFont(fonts_.semibold, 13);
        dl->AddText(ImVec2(ix, iy), U32(c.heading), f.name.c_str());
        ImGui::PopFont();
        iy += 20 * scale_;
        text(textLayout("f" + f.name + f.value, f.value, inner, m.guildId, 14, false));
    }
    if (!e.image.proxyUrl.empty() || !e.video.url.empty()) {
        const Media& big = !e.video.url.empty() ? (e.thumbnail.width ? e.thumbnail : e.video) : e.image;
        ImVec2 size = fit(float(big.width), float(big.height), bw - pad * 2 - 4 * scale_, 300 * scale_);
        std::string still = !e.video.url.empty() ? e.thumbnail.proxyUrl : e.image.proxyUrl;
        // Only a direct video file plays here; the rest (YouTube and so on)
        // open in the browser.
        std::string play = e.video.proxyUrl;
        media(key + "m", still, play, e.url.empty() ? e.video.url : e.url, ImVec2(ix, iy + 4 * scale_), size, false, false);
        iy += size.y + 8 * scale_;
    }
    if (thumb) {
        float ts = 80 * scale_;
        ImVec2 ta(x + bw - pad - ts, y + pad);
        if (ImTextureID t = texture(e.thumbnail.proxyUrl)) {
            ImVec2 sz = fit(float(e.thumbnail.width), float(e.thumbnail.height), ts, ts);
            dl->AddImage(ImTextureRef(t), ta, ImVec2(ta.x + sz.x, ta.y + sz.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
        }
    }
    if (!e.footer.empty()) {
        ImGui::PushFont(fonts_.regular, 12);
        dl->AddText(ImVec2(ix, y + h - pad - 16 * scale_), U32(c.muted), e.footer.c_str());
        ImGui::PopFont();
    }
}

// --- reactions ---

float App::reactionsHeight(const Message& m, float w) {
    if (m.reactions.empty()) return 0;
    float x = 0, rows = 1, gap = 4 * scale_;
    ImGui::PushFont(fonts_.semibold, 14);
    for (const auto& r : m.reactions) {
        float pw = 30 * scale_ + ImGui::CalcTextSize(std::to_string(r.count).c_str()).x;
        if (x + pw > w && x > 0) {
            rows++;
            x = 0;
        }
        x += pw + gap;
    }
    ImGui::PopFont();
    return rows * (26 * scale_ + gap) + 2 * scale_;
}

void App::reactions(const Message& m, float x, float y, float w) {
    if (m.reactions.empty()) return;
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cx = 0, cy = y + 4 * scale_, gap = 4 * scale_, ph = 26 * scale_, img = 16 * scale_;
    bool canReact = !guild_ || (perms(m.channelId) & perm::AddReactions);
    ImGui::PushFont(fonts_.semibold, 14);
    int i = 0;
    for (const auto& r : m.reactions) {
        std::string n = std::to_string(r.count);
        float pw = 30 * scale_ + ImGui::CalcTextSize(n.c_str()).x;
        if (cx + pw > w && cx > 0) {
            cy += ph + gap;
            cx = 0;
        }
        ImVec2 a(x + cx, cy), b(a.x + pw, a.y + ph);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(int(m.id & 0x7FFFFFFF));
        ImGui::PushID(i++);
        bool clicked = ImGui::InvisibleButton("r", ImVec2(pw, ph));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::PopID();
        dl->AddRectFilled(a, b, r.me ? U32(c.accent, 0.25f) : U32(c.code));
        dl->AddRect(a, b, r.me ? U32(c.accent) : (hov ? U32(c.muted) : U32(c.code)));
        std::string url = r.emoji.id ? "https://cdn.discordapp.com/emojis/" + IdString(r.emoji.id) +
                                           (r.emoji.animated ? ".gif" : ".png") + "?size=48"
                                     : "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + TwemojiName(r.emoji.name) + ".png";
        ImVec2 ia(a.x + 6 * scale_, a.y + (ph - img) / 2);
        if (ImTextureID t = image(url, r.emoji.animated && hovered_ == m.id))
            dl->AddImage(ImTextureRef(t), ia, ImVec2(ia.x + img, ia.y + img));
        dl->AddText(ImVec2(ia.x + img + 5 * scale_, a.y + (ph - ImGui::GetFontSize()) / 2), r.me ? U32(c.heading) : U32(c.muted), n.c_str());
        if (hov) {
            // By name: the emoji itself isn't in the font, so it would
            // leave the tooltip blank and too wide.
            std::string name = r.emoji.name;
            if (!r.emoji.id)
                if (const EmojiEntry* e = EmojiByChar(r.emoji.name)) name = FirstName(*e);
            ImGui::SetTooltip(":%s:", name.c_str());
        }
        if (clicked && canReact && !m.pending) react(m.channelId, m.id, r.emoji, !r.me);
        cx += pw + gap;
    }
    ImGui::PopFont();
}

// --- composer ---

int App::composerCallback(ImGuiInputTextCallbackData* d) {
    App* app = static_cast<App*>(d->UserData);
    if (!app->pendingInsert_.empty() && app->pendingStart_ >= 0 && app->pendingEnd_ <= d->BufTextLen) {
        d->DeleteChars(app->pendingStart_, app->pendingEnd_ - app->pendingStart_);
        d->InsertChars(app->pendingStart_, app->pendingInsert_.c_str());
        d->CursorPos = app->pendingStart_ + int(app->pendingInsert_.size());
        d->SelectionStart = d->SelectionEnd = d->CursorPos;
        app->pendingInsert_.clear();
        app->pendingStart_ = -1;
    }
    int cur = d->CursorPos, s = cur;
    while (s > 0 && !isspace((unsigned char)d->Buf[s - 1])) s--;
    app->tokenStart_ = s;
    app->tokenEnd_ = cur;
    app->tokenText_.assign(d->Buf + s, size_t(cur - s));
    return 0;
}

bool App::suggestCommands() {
    std::string text = draft_.data();
    size_t cursor = size_t(std::max(tokenEnd_, 0));
    if (cursor < 1 || editing_ || text.find('\n') != std::string::npos) return false;
    loadCommands();
    const CommandIndex* ix = commandsHere();
    if (!ix || tokenStart_ == dismissedToken_) return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    const size_t kMax = 10;
    size_t rest = 0;
    const Command* c = MatchCommand(ix->commands, text, commandPick_, &rest);
    if (!c || cursor <= rest) {
        // Naming the command: the ones starting with what's typed, then
        // the ones containing it.
        std::string q = lower(text.substr(1, cursor - 1));
        for (int pass = 0; pass < 2; pass++)
            for (const auto& cmd : ix->commands) {
                if (suggestions_.size() >= kMax) break;
                size_t at = cmd.path.find(q);
                if (pass == 0 ? at != 0 : (at == 0 || at == std::string::npos)) continue;
                Suggestion sg;
                sg.label = "/" + cmd.path;
                sg.insert = sg.label;
                sg.start = 0;
                sg.command = cmd.id;
                auto app = ix->apps.find(cmd.applicationId);
                sg.detail = cmd.description;
                if (app != ix->apps.end()) {
                    sg.detail += (sg.detail.empty() ? "" : "  ·  ") + app->second.name;
                    if (!app->second.icon.empty())
                        sg.image = "https://cdn.discordapp.com/app-icons/" + IdString(app->first) + "/" + app->second.icon + ".png?size=48";
                }
                suggestions_.push_back(std::move(sg));
            }
        return true;
    }

    // Its options: values for the one being written, then names to start one.
    std::string opts = text.substr(rest);
    size_t cur = cursor - rest;
    std::string name;
    size_t valueStart = 0;
    bool marked = OptionAt(*c, opts, cur, &name, &valueStart);
    if (!marked && !c->options.empty()) {
        // Before any "name:", what's written is the main option's, as
        // SplitOptions reads it.
        name = c->options[0].name;
        valueStart = std::min(opts.find_first_not_of(' '), cur);
    }
    const CommandOption* o = nullptr;
    for (const auto& x : c->options)
        if (x.name == name) o = &x;
    bool mentions = false;
    if (o) {
        std::string value = opts.substr(valueStart, cur - valueStart);
        auto offer = [&](const std::vector<CommandChoice>& choices, bool filter) {
            for (const auto& ch : choices) {
                if (suggestions_.size() >= kMax) break;
                if (filter && lower(ch.name).find(lower(value)) == std::string::npos) continue;
                Suggestion sg;
                sg.label = ch.name;
                sg.insert = ch.name;
                sg.start = int(rest + valueStart);
                sg.option = o->name;
                sg.value = ch.value;
                suggestions_.push_back(std::move(sg));
            }
        };
        if (!o->choices.empty()) {
            offer(o->choices, true);
        } else if (o->autocomplete) {
            // Ask the app once typing pauses; its answer is already filtered.
            std::string key = IdString(c->id) + " " + c->path + " " + o->name;
            if (acTyped_ != key + "\n" + value) {
                acTyped_ = key + "\n" + value;
                acTypedAt_ = now_;
            }
            if (acAsked_ != acTyped_ && now_ - acTypedAt_ > 0.3) {
                acAsked_ = acTyped_;
                requestAutocomplete(*c, opts, o->name);
            }
            if (acFor_ == key) offer(acChoices_, false);
        } else {
            mentions = o->type == opt::User || o->type == opt::Channel || o->type == opt::Role || o->type == opt::Mentionable;
        }
    }
    size_t values = suggestions_.size();

    // Names, where one could start: anywhere but a named value's first word.
    std::string word = lower(tokenText_);
    bool firstWord = size_t(tokenStart_) <= rest + valueStart;
    if (word.find(':') == std::string::npos && (!marked || (!firstWord && !word.empty())))
        for (const auto& op : c->options) {
            // Named already ("name:"); a bare word could still be the start of one.
            std::string lo = " " + lower(opts), mark = lower(op.name) + ":";
            bool used = lo.find(" " + mark) != std::string::npos || lo.find("\n" + mark) != std::string::npos;
            if (used || op.name.rfind(word, 0) != 0 || suggestions_.size() >= kMax) continue;
            Suggestion sg;
            sg.label = op.name;
            sg.insert = op.name + ":";
            sg.detail = op.description + (op.required ? "  (required)" : "");
            suggestions_.push_back(std::move(sg));
        }
    // Naming an option is Tab's, so Enter can send what's written.
    passiveSuggestions_ = values == 0 && !suggestions_.empty();
    if (suggestions_.empty() && mentions) return false;  // @ and # suggest as usual
    return true;
}

void App::suggest() {
    suggestions_.clear();
    passiveSuggestions_ = false;
    if (draft_[0] == '/' && suggestCommands()) {
        if (suggestion_ >= int(suggestions_.size())) suggestion_ = 0;
        return;
    }
    if (tokenText_.size() < 2 && !(tokenText_ == "@")) return;
    if (tokenStart_ == dismissedToken_) return;
    char kind = tokenText_[0];
    std::string q = tokenText_.substr(1);
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    const size_t kMax = 8;
    if (kind == '@') {
        std::vector<uint64_t> people;
        if (guild_) {
            for (const auto& [uid, mem] : store_.guilds[guild_].members) people.push_back(uid);
        } else if (store_.channels.count(channel_)) {
            people = store_.channels[channel_].recipients;
        }
        // Recent authors first.
        std::vector<uint64_t> recent;
        const auto& list = store_.messages[channel_].list;
        for (auto it = list.rbegin(); it != list.rend() && recent.size() < 50; ++it)
            if (std::find(recent.begin(), recent.end(), it->authorId) == recent.end()) recent.push_back(it->authorId);
        std::stable_sort(people.begin(), people.end(), [&](uint64_t a, uint64_t b) {
            auto ia = std::find(recent.begin(), recent.end(), a), ib = std::find(recent.begin(), recent.end(), b);
            return ia < ib;
        });
        for (uint64_t uid : people) {
            if (suggestions_.size() >= kMax) break;
            if (uid == store_.me) continue;
            std::string name = store_.DisplayName(guild_, uid);
            const User* u = store_.FindUser(uid);
            std::string user = u ? u->username : "";
            if (!q.empty() && lower(name).find(q) == std::string::npos && lower(user).find(q) == std::string::npos) continue;
            suggestions_.push_back({name + (user.empty() || user == name ? "" : "  " + user), "@" + name, "<@" + IdString(uid) + ">",
                                    avatarUrl(uid, guild_), userColour(guild_, uid)});
        }
        if (guild_ && (perms(channel_) & perm::MentionEveryone))
            for (const char* e : {"everyone", "here"})
                if (suggestions_.size() < kMax && std::string(e).rfind(q, 0) == 0)
                    suggestions_.push_back({std::string("@") + e, std::string("@") + e, std::string("@") + e, "", 0});
    } else if (kind == ':' && q.size() >= 2) {
        if (guild_)
            for (const auto& e : store_.guilds[guild_].emojis)
                if (suggestions_.size() < kMax && lower(e.name).find(q) != std::string::npos)
                    suggestions_.push_back({":" + e.name + ":", ":" + e.name + ":", "",
                                            "https://cdn.discordapp.com/emojis/" + IdString(e.id) + ".png?size=48", 0});
        for (int pass = 0; pass < 2 && suggestions_.size() < kMax; pass++)
            for (const auto& e : EmojiTable()) {
                if (suggestions_.size() >= kMax) break;
                std::string names = std::string(" ") + e.names;
                bool prefix = names.find(" " + q) != std::string::npos;
                if (pass == 0 ? !prefix : (prefix || names.find(q) == std::string::npos)) continue;
                suggestions_.push_back({":" + FirstName(e) + ":", e.emoji, "",
                                        "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + TwemojiName(e.emoji) + ".png", 0});
            }
    } else if (kind == '#' && guild_) {
        for (uint64_t id : store_.GuildChannels(guild_)) {
            const Channel& ch = store_.channels[id];
            if (ch.type == chan::Category || suggestions_.size() >= kMax) continue;
            if (!q.empty() && lower(ch.name).find(q) == std::string::npos) continue;
            suggestions_.push_back({"#" + ch.name, "#" + ch.name, "<#" + IdString(id) + ">", "", 0});
        }
    }
    if (suggestion_ >= int(suggestions_.size())) suggestion_ = 0;
}

void App::pick(const Suggestion& s) {
    pendingInsert_ = s.insert + " ";
    pendingStart_ = s.start >= 0 ? s.start : tokenStart_;
    pendingEnd_ = tokenEnd_;
    if (s.command) commandPick_ = s.command;
    if (!s.option.empty()) commandPicked_[s.option][s.insert] = s.value;
    if (!s.code.empty() && s.code != s.insert) mentionCodes_[s.insert] = s.code;
    suggestions_.clear();
}

void App::autocomplete(ImVec2 above, float w) {
    if (suggestions_.empty()) return;
    const Palette& c = Colours();
    float row = 34 * scale_, pad = 6 * scale_;
    float h = suggestions_.size() * row + pad * 2;
    ImVec2 a(above.x, above.y - h - 6 * scale_);
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));  // rows exactly row apart
    ImGui::PushStyleColor(ImGuiCol_WindowBg, c.sidebar);
    ImGui::Begin("##suggest", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_Tooltip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < int(suggestions_.size()); i++) {
        const Suggestion& s = suggestions_[i];
        ImVec2 ra = ImGui::GetCursorScreenPos(), rb(ra.x + w - pad * 2, ra.y + row);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("s", ImVec2(w - pad * 2, row));
        if (ImGui::IsItemHovered()) suggestion_ = i;
        ImGui::PopID();
        if (i == suggestion_) dl->AddRectFilled(ra, rb, U32(c.selected));
        float tx = ra.x + 8 * scale_, img = 22 * scale_;
        if (!s.image.empty()) {
            if (ImTextureID t = texture(s.image)) {
                ImVec2 ia(tx, ra.y + (row - img) / 2);
                bool round = s.colour != 0;
                dl->AddImage(ImTextureRef(t), ia, ImVec2(ia.x + img, ia.y + img), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
            }
            tx += img + 10 * scale_;
        }
        float ty = ra.y + (row - ImGui::GetFontSize()) / 2;
        dl->AddText(ImVec2(tx, ty), s.colour ? s.colour : U32(c.text), s.label.c_str());
        if (!s.detail.empty()) {
            float dx = tx + ImGui::CalcTextSize(s.label.c_str()).x + 14 * scale_;
            ImGui::PushClipRect(ImVec2(dx, ra.y), ImVec2(rb.x - 8 * scale_, rb.y), true);
            dl->AddText(ImVec2(dx, ty), U32(c.muted), s.detail.c_str());
            ImGui::PopClipRect();
        }
        if (clicked) pick(s);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::composer(float x, float y, float w, float* height) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float side = 16 * scale_, used = 0;
    float boxX = o.x + side, boxW = w - side * 2;
    float top = o.y + y;

    if (!canSend(channel_)) {
        float bh = 32 * scale_;
        ImVec2 a(boxX, top);
        dl->AddRectFilled(a, ImVec2(a.x + boxW, a.y + bh), U32(c.input));
        dl->AddText(ImVec2(a.x + 16 * scale_, a.y + (bh - ImGui::GetFontSize()) / 2), U32(c.muted),
                    "You can't send messages in this channel.");
        *height = bh + 18 * scale_;
        return;
    }

    // Bars above the box: replying, editing, attachments, errors.
    float barH = 24 * scale_;
    auto bar = [&](const std::string& text, bool* cancel) {
        ImVec2 a(boxX, top + used);
        dl->AddRectFilled(a, ImVec2(a.x + boxW, a.y + barH), U32(c.sidebar));
        ImGui::PushFont(fonts_.regular, 13);
        dl->AddText(ImVec2(a.x + 14 * scale_, a.y + (barH - ImGui::GetFontSize()) / 2), U32(c.muted), text.c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(a.x + boxW - 28 * scale_, a.y + 3 * scale_));
        *cancel = iconButton(text.c_str(), icon::Dismiss, ImVec2(24 * scale_, 24 * scale_), 11, fonts_.regular);
        used += barH;
    };
    if (replyTo_) {
        std::string name;
        for (const auto& m : store_.messages[channel_].list)
            if (m.id == replyTo_) name = store_.DisplayName(guild_, m.authorId);
        bool cancel = false;
        bar("Replying to " + name, &cancel);
        if (cancel) replyTo_ = 0;
    }
    if (editing_) {
        bool cancel = false;
        bar("Editing a message: Enter saves, Escape cancels", &cancel);
        if (cancel) {
            editing_ = 0;
            std::fill(draft_.begin(), draft_.end(), 0);
            if (auto it = drafts_.find(channel_); it != drafts_.end()) strncpy(draft_.data(), it->second.c_str(), draft_.size() - 1);
        }
    }
    if (!files_.empty()) {
        float fh = 34 * scale_;
        float fx = boxX;
        ImGui::PushFont(fonts_.regular, 13);
        for (size_t i = 0; i < files_.size(); i++) {
            std::string label = files_[i].name + "  " + fileSize(files_[i].bytes.size());
            float fw = ImGui::CalcTextSize(label.c_str()).x + 44 * scale_;
            if (fx + fw > boxX + boxW && fx > boxX) {
                fx = boxX;
                used += fh + 4 * scale_;
            }
            ImVec2 a(fx, top + used);
            dl->AddRectFilled(a, ImVec2(a.x + fw, a.y + fh), U32(c.sidebar));
            dl->AddText(ImVec2(a.x + 10 * scale_, a.y + (fh - ImGui::GetFontSize()) / 2), U32(c.text), label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(a.x + fw - 28 * scale_, a.y + 5 * scale_));
            ImGui::PushID(int(i));
            if (iconButton("x", icon::Dismiss, ImVec2(24 * scale_, 24 * scale_), 11, fonts_.regular, "Remove")) {
                files_.erase(files_.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
            fx += fw + 6 * scale_;
        }
        ImGui::PopFont();
        used += fh + 6 * scale_;
    }
    // The command being written: its options (optional ones bracketed),
    // and what the one at the cursor is for.
    size_t cmdRest = 0;
    if (const Command* cmd = typedCommand(&cmdRest)) {
        std::string line = "/" + cmd->path, about = cmd->description, name;
        size_t vs = 0;
        std::string typed = draft_.data();
        bool inOption = tokenEnd_ >= int(cmdRest) &&
                        OptionAt(*cmd, typed.substr(cmdRest), size_t(tokenEnd_) - cmdRest, &name, &vs);
        for (const auto& o : cmd->options) {
            line += "   " + std::string(o.required ? "" : "[") + o.name + (o.required ? "" : "]");
            if (inOption && o.name == name) about = o.name + ": " + o.description;
        }
        if (!about.empty()) line += "   —   " + about;
        bool cancel = false;
        bar(line, &cancel);
        if (cancel) {
            std::fill(draft_.begin(), draft_.end(), 0);
            commandPicked_.clear();
            focusComposer_ = true;
        }
    }
    if (!commandStatus_.empty() && now_ - commandSentAt_ > 20) commandStatus_.clear();
    if (!commandStatus_.empty()) {
        ImGui::PushFont(fonts_.regular, 13);
        dl->AddText(ImVec2(boxX + 4 * scale_, top + used + 2 * scale_), U32(c.muted), commandStatus_.c_str());
        ImGui::PopFont();
        used += 22 * scale_;
    }
    if (!composeError_.empty()) {
        ImGui::PushFont(fonts_.regular, 13);
        dl->AddText(ImVec2(boxX + 4 * scale_, top + used + 2 * scale_), U32(c.bad), composeError_.c_str());
        ImGui::PopFont();
        used += 22 * scale_;
    }

    // The box: attach button, text, emoji button.
    float btn = 32 * scale_, base = 32 * scale_;  // the box's height with one line
    float textW = boxW - btn * 2;
    float textH = ImGui::CalcTextSize(draft_.data(), nullptr, false, textW - 20 * scale_).y;
    float lineH = ImGui::GetFontSize();
    // One line keeps the empty box's height, so typing doesn't nudge
    // everything above it; only a second line grows it.
    float boxH = base;
    if (textH > lineH * 1.5f)
        boxH = std::clamp(textH + (base - lineH) + 1, base, std::max(base, ImGui::GetMainViewport()->Size.y * 0.4f));
    ImVec2 a(boxX, top + used);
    dl->AddRectFilled(a, ImVec2(a.x + boxW, a.y + boxH), U32(c.input));
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + (base - btn) / 2));
    if (iconButton("attach", icon::Attach, ImVec2(btn, btn), 15, fonts_.regular, "Attach files")) {
        wchar_t buf[8192] = {};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = ARRAYSIZE(buf);
        ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            std::wstring dir = buf;
            const wchar_t* p = buf + dir.size() + 1;
            if (!*p) {
                attachFile(dir);
            } else {
                for (; *p; p += wcslen(p) + 1) attachFile(dir + L"\\" + p);
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + boxW - btn, a.y + (base - btn) / 2));
    if (iconButton("emoji", icon::Emoji, ImVec2(btn, btn), 15, fonts_.regular, "Emoji")) {
        pickerMessage_ = 0;
        pickerAt_ = ImVec2(a.x + boxW, a.y);
        pickerOpen_ = true;
    }

    // A click anywhere in the box, beside the text too, is for writing.
    // (Clicks on the text box itself are its own, to place the cursor.)
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(a.x + btn, a.y), ImVec2(a.x + boxW - btn, a.y + boxH)) &&
        !composerRect_.Contains(ImGui::GetMousePos()))
        focusComposer_ = true;

    // Keys the text box mustn't see: picking a suggestion, pasting an
    // image, editing the last message.
    ImGuiID inputId = ImGui::GetID("##draft");
    bool active = ImGui::GetActiveID() == inputId;
    suggest();
    if (active && !suggestions_.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) suggestion_ = (suggestion_ + 1) % int(suggestions_.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) suggestion_ = (suggestion_ + int(suggestions_.size()) - 1) % int(suggestions_.size());
        bool choose = ImGui::IsKeyPressed(ImGuiKey_Tab) ||
                      (!passiveSuggestions_ && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            dismissedToken_ = tokenStart_;
            suggestions_.clear();
        }
        for (ImGuiKey k : {ImGuiKey_DownArrow, ImGuiKey_UpArrow, ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
            if (!passiveSuggestions_ || (k != ImGuiKey_Enter && k != ImGuiKey_KeypadEnter))
                ImGui::SetKeyOwner(k, ImGui::GetID("suggest"), ImGuiInputFlags_LockThisFrame);
        if (choose && !suggestions_.empty()) pick(suggestions_[suggestion_]);
    } else if (active) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (editing_) {
                editing_ = 0;
                std::fill(draft_.begin(), draft_.end(), 0);
                if (auto it = drafts_.find(channel_); it != drafts_.end()) strncpy(draft_.data(), it->second.c_str(), draft_.size() - 1);
            } else {
                replyTo_ = 0;
            }
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("esc"), ImGuiInputFlags_LockThisFrame);
            focusComposer_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && draft_[0] == 0 && !editing_) {
            const auto& list = store_.messages[channel_].list;
            for (auto it = list.rbegin(); it != list.rend(); ++it)
                if (it->authorId == store_.me && !it->pending && normalType(it->type)) {
                    drafts_[channel_] = "";
                    strncpy(draft_.data(), it->content.c_str(), draft_.size() - 1);
                    editing_ = it->id;
                    replyTo_ = 0;
                    break;
                }
            ImGui::SetKeyOwner(ImGuiKey_UpArrow, ImGui::GetID("up"), ImGuiInputFlags_LockThisFrame);
            focusComposer_ = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) &&
            (IsClipboardFormatAvailable(CF_HDROP) || !IsClipboardFormatAvailable(CF_UNICODETEXT)) && pasteImage())
            ImGui::SetKeyOwner(ImGuiKey_V, ImGui::GetID("paste"), ImGuiInputFlags_LockThisFrame);
    }
    if (!suggestions_.empty() && active) autocomplete(ImVec2(a.x, top), boxW);  // above the bars too

    // Type anywhere to write.
    if (!active && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        GetForegroundWindow() == hwnd_ && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().InputQueueCharacters.empty())
        focusComposer_ = true;

    // The text box fills the box exactly, its padding centring one line,
    // so it never needs a scrollbar until it's at its tallest.
    float padY = (base - lineH) / 2;
    ImGui::SetCursorScreenPos(ImVec2(a.x + btn, a.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * scale_, padY));
    if (focusComposer_) {
        ImGui::SetKeyboardFocusHere();
        focusComposer_ = false;
    }
    bool sendIt = ImGui::InputTextMultiline("##draft", draft_.data(), draft_.size(), ImVec2(textW, boxH),
                                            ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackAlways |
                                                ImGuiInputTextFlags_NoHorizontalScroll,
                                            &App::composerCallback, this);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    composerActive_ = ImGui::IsItemActive();
    composerRect_ = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (draft_[0] == 0 && !composerActive_) {
        std::string hint = editing_ ? "Edit message" : "Message " + channelTitle(channel_);
        dl->AddText(ImVec2(a.x + btn + 4 * scale_, a.y + (base - lineH) / 2), U32(c.muted), hint.c_str());
    } else if (draft_[0] == 0) {
        std::string hint = "Message " + channelTitle(channel_);
        dl->AddText(ImVec2(a.x + btn + 4 * scale_, a.y + (base - lineH) / 2), U32(c.muted, 0.6f), hint.c_str());
    }
    if (sendIt) {
        send();
        focusComposer_ = true;  // Enter leaves the box; come straight back
    }
    used += boxH;

    // Tell Discord we're typing, as its app does: on a change to the box,
    // at most every nine seconds, not while editing.
    size_t hash = std::hash<std::string>()(draft_.data());
    if (hash != draftHash_) {
        draftHash_ = hash;
        unsigned long long now = GetTickCount64();
        if (draft_[0] && !editing_ && draft_[0] != '/' && now - typingSent_ > 9000 && api_) {
            typingSent_ = now;
            Api* api = api_.get();
            uint64_t ch = channel_;
            sender_->Run([api, ch] { api->Call("POST", "/channels/" + IdString(ch) + "/typing"); });
        }
    }

    // Who else is typing, in the strip under the box.
    std::string typing = typingLine(channel_);
    if (!typing.empty()) {
        ImGui::PushFont(fonts_.regular, 12);
        dl->AddText(ImVec2(boxX + 4 * scale_, top + used + 2 * scale_), U32(c.muted), typing.c_str());
        ImGui::PopFont();
    }
    *height = used + 18 * scale_;
}

}  // namespace disky
