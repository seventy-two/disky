#include "theme.h"

#include <windows.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace disky {

ImVec4 Hex(unsigned rgb, float a) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

ImU32 U32(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

namespace {

// Close to Discord's own dark and light themes, so it reads as familiar.
const Palette kDark = {
    Hex(0x1E1F22), Hex(0x2B2D31), Hex(0x313338), Hex(0xDBDEE1), Hex(0x949BA4), Hex(0xF2F3F5), Hex(0x5865F2),
    Hex(0x00A8FC), Hex(0x383A40), Hex(0x2B2D31), Hex(0x1F2023), Hex(0x2E3035), Hex(0x404249), Hex(0x23A55A),
    Hex(0xF0B232), Hex(0xF23F43), Hex(0xDA373C), Hex(0xF0B232, 0.08f), Hex(0xF0B232), Hex(0x1E1F22),
};

const Palette kLight = {
    Hex(0xE3E5E8), Hex(0xF2F3F5), Hex(0xFFFFFF), Hex(0x313338), Hex(0x5C5E66), Hex(0x060607), Hex(0x5865F2),
    Hex(0x006CE7), Hex(0xEBEDEF), Hex(0xF2F3F5), Hex(0xE1E2E4), Hex(0xF5F5F6), Hex(0xD7D9DC), Hex(0x23A55A),
    Hex(0xF0B232), Hex(0xF23F43), Hex(0xDA373C), Hex(0xF0B232, 0.12f), Hex(0xC88A00), Hex(0xB5B8BD),
};

Palette current = kDark;

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// fontData reads a font file once, so fonts merged into several faces
// (icons, symbols) share one copy in memory.
std::vector<char>* fontData(const std::string& path) {
    static std::map<std::string, std::vector<char>> files;
    auto it = files.find(path);
    if (it != files.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<char>& data = files[path];
    std::ifstream f(path, std::ios::binary);
    if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return data.empty() ? nullptr : &data;
}

ImFont* addFont(const std::string& path, ImFontConfig cfg, const ImWchar* ranges = nullptr) {
    std::vector<char>* data = fontData(path);
    if (!data) return nullptr;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data->data(), int(data->size()), kBody, &cfg, ranges);
}

}  // namespace

bool SystemUsesDarkTheme() {
    wchar_t forced[16];
    if (GetEnvironmentVariableW(L"DISKY_THEME", forced, ARRAYSIZE(forced))) return wcscmp(forced, L"light") != 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

const Palette& Colours() { return current; }

void ApplyTheme(bool dark, float scale) {
    current = dark ? kDark : kLight;
    const Palette& p = current;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 4);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 8;
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 4;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.FontSizeBase = kBody;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    ImVec4* c = s.Colors;
    ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.sidebar;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.input;
    c[ImGuiCol_FrameBgHovered] = p.input;
    c[ImGuiCol_FrameBgActive] = p.input;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = p.rail;
    c[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    c[ImGuiCol_ScrollbarGrabActive] = p.muted;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.selected;
    c[ImGuiCol_ButtonHovered] = p.hover;
    c[ImGuiCol_ButtonActive] = p.border;
    c[ImGuiCol_Header] = p.selected;
    c[ImGuiCol_HeaderHovered] = p.hover;
    c[ImGuiCol_HeaderActive] = p.selected;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.6f : 0.35f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // no keyboard focus outline
}

Fonts LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    // Symbols Segoe UI lacks (arrows, maths, box drawing) come from Segoe
    // UI Symbol. CJK fonts are left out: each is tens of MB in memory.
    std::string symbols = fontPath("seguisym.ttf");

    auto add = [&](const char* file, bool icons) -> ImFont* {
        ImFont* f = addFont(fontPath(file), ImFontConfig());
        if (!f) return io.Fonts->AddFontDefault();
        if (icons) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = ImVec2(0, 1);
            static const ImWchar range[] = {0xE700, 0xF8FF, 0};
            addFont(iconFont, merge, range);
        }
        ImFontConfig merge;
        merge.MergeMode = true;
        addFont(symbols, merge);
        return f;
    };
    Fonts f;
    f.regular = add("segoeui.ttf", true);
    f.bold = add("segoeuib.ttf", false);
    f.italic = add("segoeuii.ttf", false);
    f.boldItalic = add("segoeuiz.ttf", false);
    f.semibold = add("seguisb.ttf", true);
    f.mono = add("consola.ttf", false);
    io.FontDefault = f.regular;
    return f;
}

}  // namespace disky
