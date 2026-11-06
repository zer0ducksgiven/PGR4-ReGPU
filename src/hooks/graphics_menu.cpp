// The Options > Graphics screen: the 360's TV calibration picture is replaced with the PC's resolution, display mode,
// anti-aliasing, texture filtering and frame cap, as a spinner box in the style of the game's Advanced Sound screen (Lua in GraphicsMenuPatch,
// run after Options\Graphics.lua). Its texts are this port's own: text ids from kTextBase are answered by the
// game's string lookup hook below. Saving reports the choice through a caught Lua error ("PGR4SET ...", seen by
// the luaD_throw hook in lua.cpp), and all of them apply at once.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <regpu/regpu.h>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

namespace
{
constexpr uint32_t kTextBase = 0xF0C0A000;

std::mutex g_textLock;
std::map<uint32_t, uint32_t> g_texts; // text id -> guest UTF-16BE string
std::map<std::string, uint32_t> g_textIds;

std::vector<std::pair<uint32_t, uint32_t>> g_resolutions;
const uint32_t kMsaa[] = { 1, 2, 4, 8 };
const int kAniso[] = { -1, 0, 2, 4, 8, 16 }; // as the launcher lists them
std::vector<uint32_t> g_fps;

// A text id for this string (made once), for the game's UI.
uint32_t Text(const std::string& s)
{
    std::lock_guard lock(g_textLock);
    if (auto it = g_textIds.find(s); it != g_textIds.end())
        return it->second;
    const uint32_t guest = GuestAlloc(Heap::Virtual, uint32_t(s.size() * 2 + 2), 16);
    if (!guest)
        return 0;
    uint8_t* p = GuestPtr<uint8_t>(guest);
    for (size_t i = 0; i <= s.size(); i++)
    {
        const uint16_t c = i < s.size() ? uint8_t(s[i]) : 0;
        p[i * 2] = uint8_t(c >> 8);
        p[i * 2 + 1] = uint8_t(c);
    }
    const uint32_t id = kTextBase + uint32_t(g_texts.size());
    g_texts[id] = guest;
    g_textIds[s] = id;
    return id;
}

// The display's modes from 1280x720 up (as the launcher lists them), and the current setting.
void ListResolutions()
{
    std::set<std::pair<uint32_t, uint32_t>> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720)
            modes.insert({ dm.dmPelsWidth, dm.dmPelsHeight });
    const Settings& s = GetSettings();
    modes.insert({ s.width, s.height });
    g_resolutions.assign(modes.begin(), modes.end());
    std::sort(g_resolutions.begin(), g_resolutions.end(),
        [](auto a, auto b) { return uint64_t(a.first) * a.second < uint64_t(b.first) * b.second || (uint64_t(a.first) * a.second == uint64_t(b.first) * b.second && a.first < b.first); });
}
}

// The game's text lookup (loc, hash) -> UTF-16BE string, or 0.
PPC_FUNC_IMPL(__imp__sub_8289B1D0);
PPC_FUNC(sub_8289B1D0)
{
    const uint32_t id = ctx.r4.u32;
    if (id >= kTextBase && id < kTextBase + 0x1000)
    {
        std::lock_guard lock(g_textLock);
        if (auto it = g_texts.find(id); it != g_texts.end())
        {
            ctx.r3.u64 = it->second;
            return;
        }
    }
    __imp__sub_8289B1D0(ctx, base);
}

std::string GraphicsMenuPatch()
{
    ListResolutions();
    // The launcher's caps, and the current one if it set a custom cap.
    g_fps = { 30, 60, 120, 144, 165, 240, 360 };
    if (std::find(g_fps.begin(), g_fps.end(), GetSettings().maxFps) == g_fps.end())
    {
        g_fps.push_back(GetSettings().maxFps);
        std::sort(g_fps.begin(), g_fps.end());
    }
    const Settings& s = GetSettings();
    std::string res, aa;
    size_t resNow = 0, aaNow = 0;
    for (size_t i = 0; i < g_resolutions.size(); i++)
    {
        const auto [w, h] = g_resolutions[i];
        res += std::to_string(Text(std::to_string(w) + " X " + std::to_string(h))) + ",";
        if (w == s.width && h == s.height)
            resNow = i;
    }
    for (size_t i = 0; i < std::size(kMsaa); i++)
    {
        aa += std::to_string(Text(kMsaa[i] == 1 ? "OFF" : std::to_string(kMsaa[i]) + "X MSAA")) + ",";
        if (kMsaa[i] == s.msaa)
            aaNow = i;
    }
    std::string af, fps;
    size_t afNow = 0, fpsNow = 0;
    for (size_t i = 0; i < std::size(kAniso); i++)
    {
        const int v = kAniso[i];
        af += std::to_string(Text(v < 0 ? "GAME DEFAULT" : v == 0 ? "OFF" : std::to_string(v) + "X")) + ",";
        if (v == s.anisotropy)
            afNow = i;
    }
    for (size_t i = 0; i < g_fps.size(); i++)
    {
        fps += std::to_string(Text(std::to_string(g_fps[i]) + " FPS")) + ",";
        if (g_fps[i] == s.maxFps)
            fpsNow = i;
    }
    const std::string mode = std::to_string(Text("WINDOWED")) + "," + std::to_string(Text("FULLSCREEN")) + ",";
    std::string lua = "local RES = {" + res + "}\nlocal AA = {" + aa + "}\nlocal MODE = {" + mode + "}\n";
    lua += "local RES_NOW, AA_NOW, MODE_NOW = " + std::to_string(resNow) + ", " + std::to_string(aaNow) + ", " + (s.fullscreen ? "1" : "0") + "\n";
    lua += "local AF = {" + af + "}\nlocal FPS = {" + fps + "}\nlocal AF_NOW, FPS_NOW = " + std::to_string(afNow) + ", " + std::to_string(fpsNow) + "\n";
    lua += "local T_AF, T_FPS = " + std::to_string(Text("ANISOTROPIC FILTERING")) + ", " + std::to_string(Text("FRAME RATE CAP")) + "\n";
    lua += "local T_RES, T_AA, T_MODE = " + std::to_string(Text("RESOLUTION")) + ", " + std::to_string(Text("ANTI-ALIASING")) + ", " +
        std::to_string(Text("DISPLAY MODE")) + "\n";
    lua += R"lua(
GUI = { finished = false, optionChosen = 0, maxOption = 4 }
function Init()
    SetupFadePanel()
    SetupTitle(UIText.IDS_GRAPHICS)
    SetupScreenTimers()
    SetupSlideMenuInfo()
    SetupInfoText(UIText.IDS_CANCEL_B_SAVE_A, 1, false)
    local frame = GetDefaultFrame(113.33333333333334, UIGlobals.screen.safe_top + 40, 453.3333333333333, UIGlobals.screen.safe_bottom - 110)
    frame.panel = 1
    UIButtons.AddButton(frame)
    SetupFrameTitle(frame, UIText.IDS_GRAPHICS)
    local y = frame.y + UIGlobals.frame_mainbox_offset_y + 12
    GUI.lower_clip = frame.height - y + 16
    SetClipToFrame(GUIBank.variable_spinner, frame)
    GUI.res_ID = SetupVariableSpinner(T_RES, y, 320, 174, 1)
    for i = #RES, 1, -1 do UIButtons.AddItem(GUI.res_ID, i - 1, RES[i], false) end
    y = y + UIGlobals.options_cr
    GUI.aa_ID = SetupVariableSpinner(T_AA, y, 320, 174, 1)
    for i = #AA, 1, -1 do UIButtons.AddItem(GUI.aa_ID, i - 1, AA[i], false) end
    y = y + UIGlobals.options_cr
    GUI.mode_ID = SetupVariableSpinner(T_MODE, y, 320, 174, 1)
    for i = #MODE, 1, -1 do UIButtons.AddItem(GUI.mode_ID, i - 1, MODE[i], false) end
    y = y + UIGlobals.options_cr
    GUI.af_ID = SetupVariableSpinner(T_AF, y, 320, 174, 1)
    for i = #AF, 1, -1 do UIButtons.AddItem(GUI.af_ID, i - 1, AF[i], false) end
    y = y + UIGlobals.options_cr
    GUI.fps_ID = SetupVariableSpinner(T_FPS, y, 320, 174, 1)
    for i = #FPS, 1, -1 do UIButtons.AddItem(GUI.fps_ID, i - 1, FPS[i], false) end
    UIButtons.SetSelection(GUI.af_ID, AF_NOW)
    UIButtons.SetSelection(GUI.fps_ID, FPS_NOW)
    UIButtons.SetSelection(GUI.res_ID, RES_NOW)
    UIButtons.SetSelection(GUI.aa_ID, AA_NOW)
    UIButtons.SetSelection(GUI.mode_ID, MODE_NOW)
    ActivateOptions()
end
function StartLoop() return true end
function ActivateOptions()
    UIButtons.SetSelected(GUI.res_ID, GUI.optionChosen == 0)
    UIButtons.SetSelected(GUI.aa_ID, GUI.optionChosen == 1)
    UIButtons.SetSelected(GUI.mode_ID, GUI.optionChosen == 2)
    UIButtons.SetSelected(GUI.af_ID, GUI.optionChosen == 3)
    UIButtons.SetSelected(GUI.fps_ID, GUI.optionChosen == 4)
end
function MessageUpdate(msg)
    if GUI.finished == true then return end
    if msg == "BUTTON_BACK" then
        GUI.finished = true
        EndFadePanel(false)
        PlayMenuBack()
        UIScreen.SetNextScreen("Options\\MainMenu.lua")
    elseif msg == "BUTTON_UP" or msg == "BUTTON_DOWN" then
        PlayMenuOptionMove()
        local n = GUI.optionChosen + (msg == "BUTTON_UP" and -1 or 1)
        if n < 0 then n = GUI.maxOption elseif n > GUI.maxOption then n = 0 end
        GUI.optionChosen = n
        ActivateOptions()
    elseif msg == "BUTTON_NEXT" then
        GUI.finished = true
        EndFadePanel(true)
        PlayMenuNext()
        pcall(error, "PGR4SET " .. UIButtons.GetSelection(GUI.res_ID) .. " " .. UIButtons.GetSelection(GUI.aa_ID) .. " " .. UIButtons.GetSelection(GUI.mode_ID) .. " " .. UIButtons.GetSelection(GUI.af_ID) .. " " .. UIButtons.GetSelection(GUI.fps_ID), 0)
        UIScreen.SetNextScreen("Options\\MainMenu.lua")
    end
end
function FrameUpdate() end
function Render() end
function End() end
function EndLoop() return true end
)lua";
    return lua;
}

// "PGR4SET <resolution> <anti-aliasing> <display mode: 0 windowed, 1 fullscreen> <filtering> <frame cap>" (indices) from the screen's Save.
bool GraphicsMenuCommand(const char* text, size_t len)
{
    constexpr char kTag[] = "PGR4SET ";
    if (len < sizeof kTag - 1 || memcmp(text, kTag, sizeof kTag - 1) != 0)
        return false;
    double r = -1, a = -1, m = -1, f = -1, c = -1;
    const std::string args(text + sizeof kTag - 1, len - (sizeof kTag - 1));
    if (sscanf(args.c_str(), "%lf %lf %lf %lf %lf", &r, &a, &m, &f, &c) != 5)
        return true;
    Settings& s = GetSettings();
    if (r >= 0 && size_t(r) < g_resolutions.size())
    {
        const auto [w, h] = g_resolutions[size_t(r)];
        if (w != s.width || h != s.height)
        {
            s.width = w;
            s.height = h;
            regpu::SetOutputSize(w, h);
            if (!s.fullscreen)
                regpu::SetWindowSize(w, h);
        }
    }
    if (a >= 0 && size_t(a) < std::size(kMsaa) && kMsaa[size_t(a)] != s.msaa)
    {
        s.msaa = kMsaa[size_t(a)];
        regpu::SetMsaa(s.msaa);
    }
    if ((m == 0 || m == 1) && (m == 1) != s.fullscreen) // after the size above: a new window takes it
        regpu::SetFullscreen(m == 1);
    s.fullscreen = m == 1 ? true : m == 0 ? false : s.fullscreen;
    if (f >= 0 && size_t(f) < std::size(kAniso) && kAniso[size_t(f)] != s.anisotropy)
    {
        s.anisotropy = kAniso[size_t(f)];
        regpu::SetAnisotropy(s.anisotropy);
    }
    if (c >= 0 && size_t(c) < g_fps.size() && g_fps[size_t(c)] != s.maxFps)
    {
        s.maxFps = g_fps[size_t(c)];
        regpu::SetFrameLimit(s.maxFps, s.vsync);
    }
    SaveSettings();
    Log("ui: graphics options saved: %ux%u, %s, MSAA %ux, filtering %d, cap %u fps", s.width, s.height,
        s.fullscreen ? "fullscreen" : "windowed", s.msaa, s.anisotropy, s.maxFps);
    return true;
}
