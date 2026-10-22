// Colours, fonts and ImGui style, light and dark after Windows' app theme.
#pragma once

#include "imgui.h"

namespace disky {

struct Palette {
    ImVec4 rail, sidebar, bg, text, muted, heading, accent, link, input, code, border, hover, selected;
    ImVec4 good, idle, dnd, bad, mention, mentionBar, spoiler;
};

struct Fonts {
    ImFont* regular = nullptr;  // with the icons merged in
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* boldItalic = nullptr;
    ImFont* semibold = nullptr;  // headings, names; also with icons
    ImFont* mono = nullptr;
};

// Base sizes, before DPI scaling.
constexpr float kBody = 16, kSmall = 13, kH1 = 24, kH2 = 20, kH3 = 17.5f, kJumbo = 48;

namespace icon {
inline constexpr const char* Settings = "\xEE\x9C\x93";  // U+E713
inline constexpr const char* Minimise = "\xEE\xA4\xA1";  // U+E921
inline constexpr const char* Maximise = "\xEE\xA4\xA2";  // U+E922
inline constexpr const char* Restore = "\xEE\xA4\xA3";  // U+E923
inline constexpr const char* Close = "\xEE\xA2\xBB";  // U+E8BB
inline constexpr const char* People = "\xEE\x9C\x96";  // U+E716
inline constexpr const char* Chat = "\xEE\xA2\xBD";  // U+E8BD
inline constexpr const char* Reply = "\xEE\xA3\x8A";  // U+E8CA
inline constexpr const char* Edit = "\xEE\x9C\x8F";  // U+E70F
inline constexpr const char* Delete = "\xEE\x9D\x8D";  // U+E74D
inline constexpr const char* Emoji = "\xEE\x9D\xAE";  // U+E76E
inline constexpr const char* Attach = "\xEE\x9C\xA3";  // U+E723
inline constexpr const char* Play = "\xEE\x9D\xA8";  // U+E768
inline constexpr const char* Pause = "\xEE\x9D\xA9";  // U+E769
inline constexpr const char* Volume = "\xEE\x9D\xA7";  // U+E767
inline constexpr const char* Mute = "\xEE\x9D\x8F";  // U+E74F
inline constexpr const char* Open = "\xEE\xA2\xA7";  // U+E8A7
inline constexpr const char* Dismiss = "\xEE\x9C\x91";  // U+E711
inline constexpr const char* Hash = "#";
}  // namespace icon

bool SystemUsesDarkTheme();
void ApplyTheme(bool dark, float scale);
const Palette& Colours();
// LoadFonts adds the fonts to ImGui's atlas, once at startup; with 1.92's
// dynamic atlas they're drawn at any size.
Fonts LoadFonts();

ImU32 U32(const ImVec4& c, float alpha = 1.0f);
ImVec4 Hex(unsigned rgb, float a = 1.0f);

}  // namespace disky
