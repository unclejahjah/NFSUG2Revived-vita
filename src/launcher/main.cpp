// The launcher, the VPK's eboot.bin: the start-up screen and settings manager, drawn by the CPU, then the game
// (app0:game.bin) as a fresh process through sceAppMgrLoadExec.

#include "cfgui.h"
#include "default_cfg.h"
#include "mods_edit.h"
#include "notice.h"
#include "save.h"
#include <platform/cfgfile.h>
#include <platform/log.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

using namespace ug2;

const char* const GameFolder = "ux0:data/NeedForSpeedUnderground2";
const char* const Game = "app0:game.bin";
const uint32_t FrameBytes = 2 * 1024 * 1024;    // 960 x 544 x 4 rounded up to the 256 KB CDRAM granule

uint32_t buttons(const SceCtrlData& pad)
{
    static const struct { uint32_t sce, ui; } Map[] = {
        { SCE_CTRL_UP, cfgui::BtnUp }, { SCE_CTRL_DOWN, cfgui::BtnDown }, { SCE_CTRL_LEFT, cfgui::BtnLeft },
        { SCE_CTRL_RIGHT, cfgui::BtnRight }, { SCE_CTRL_CROSS, cfgui::BtnCross }, { SCE_CTRL_CIRCLE, cfgui::BtnCircle },
        { SCE_CTRL_SQUARE, cfgui::BtnSquare }, { SCE_CTRL_TRIANGLE, cfgui::BtnTriangle },
        { SCE_CTRL_LTRIGGER, cfgui::BtnL }, { SCE_CTRL_RTRIGGER, cfgui::BtnR }, { SCE_CTRL_START, cfgui::BtnStart },
        { SCE_CTRL_SELECT, cfgui::BtnSelect },
    };
    uint32_t result = 0;
    for (const auto& m : Map)
        result |= (pad.buttons & m.sce) ? m.ui : 0;
    return result;
}

cfgui::Input readInput()
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    cfgui::Input input;
    input.buttons = buttons(pad);
    input.stickX = pad.lx;
    input.stickY = pad.ly;
    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0 && touch.reportNum > 0)
    {
        input.touch = true;
        input.touchX = touch.report[0].x / 2;
        input.touchY = touch.report[0].y / 2;
    }
    return input;
}

// Two CDRAM framebuffers and a canvas drawn in main memory.
class Screen
{
public:
    bool open()
    {
        if (m_font)
            return true;
        m_block = sceKernelAllocMemBlock("launcher", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * FrameBytes, nullptr);
        void* base = nullptr;
        m_pixels = static_cast<uint32_t*>(malloc(size_t(cfgui::Width) * cfgui::Height * 4));
        if (m_block < 0 || sceKernelGetMemBlockBase(m_block, &base) < 0 || !m_pixels)
            return false;
        m_frames[0] = static_cast<uint32_t*>(base);
        m_frames[1] = m_frames[0] + FrameBytes / 4;
        const cfgui::Font* system = cfgui::openSystemFont();
        m_font = system ? system : &cfgui::bundledFont();
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        return true;
    }

    cfgui::Canvas canvas() { return cfgui::Canvas{ m_pixels, cfgui::Width, cfgui::Height, cfgui::Width }; }
    const cfgui::Font& font() const { return *m_font; }

    void present()
    {
        uint32_t* target = m_frames[m_next];
        m_next ^= 1;
        memcpy(target, m_pixels, size_t(cfgui::Width) * cfgui::Height * 4);
        SceDisplayFrameBuf frame;
        memset(&frame, 0, sizeof(frame));
        frame.size = sizeof(frame);
        frame.base = target;
        frame.pitch = cfgui::Width;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = cfgui::Width;
        frame.height = cfgui::Height;
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
        if ((++m_count & 63) == 0)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }

private:
    SceUID m_block = -1;
    uint32_t* m_pixels = nullptr;
    uint32_t* m_frames[2] = {};
    const cfgui::Font* m_font = nullptr;
    int m_next = 0;
    unsigned m_count = 0;
};

// Shown when the game cannot be started; any button closes the launcher.
[[noreturn]] void fail(Screen& screen, const std::string& line)
{
    pf::logError("launcher: %s", line.c_str());
    if (screen.open())
    {
        uint32_t previous = ~0u;
        for (;;)
        {
            cfgui::Canvas canvas = screen.canvas();
            cfgui::drawMessage(canvas, screen.font(), { "The game could not be started.", line,
                                                         "Reinstall the VPK. Press any button to close." });
            screen.present();
            const uint32_t held = readInput().buttons;
            if (held & ~previous)
                break;
            previous = held;
        }
    }
    pf::logShutdown();
    sceKernelExitProcess(0);
    for (;;)
    {
    }
}

void configure(Screen& screen, const std::string& configPath, const std::string& text, pf::cfgfile::FileState state)
{
    if (!screen.open())
    {
        pf::logError("launcher: no memory for the screen, starting the game");
        return;
    }
    cfgui::Configurator ui(text, mods::views(GameFolder), NFSU_VERSION);
    ui.setConfigFile(configPath, state, notice::cfgUnreadable(configPath));
    ui.begin(sceKernelGetProcessTimeWide());
    cfgui::Outcome outcome;
    bool opened = false;
    for (;;)
    {
        outcome = ui.update(readInput(), sceKernelGetProcessTimeWide());
        if (outcome != cfgui::Outcome::Running)
            break;
        opened = opened || ui.screen() == cfgui::Screen::Manager;
        cfgui::Canvas canvas = screen.canvas();
        ui.draw(canvas, screen.font());
        screen.present();
    }
    if (outcome == cfgui::Outcome::SaveAndBoot)
        launcher::save(ui, configPath, state, GameFolder);
    else
        pf::logInfo("launcher: %s", opened ? "closed without saving" : "skipped");
}

}

int main()
{
    const std::string configPath = std::string(GameFolder) + "/NeedForSpeedUnderground2.cfg";
    pf::logInit((std::string(GameFolder) + "/launcher.log").c_str());
    std::string text;
    const pf::cfgfile::FileState state = pf::cfgfile::readText(configPath.c_str(), &text);
    if (state != pf::cfgfile::FileState::Readable)
        text = DefaultSettingsFile;
    if (state == pf::cfgfile::FileState::Unreadable)
        pf::logError("launcher: %s", notice::cfgUnreadable(configPath).c_str());
    std::string value;
    bool enabled = true;
    if (pf::cfgfile::find(text, "show_configurator_on_boot", &value) && !pf::cfgfile::parseBool(value.c_str(), &enabled))
        enabled = true;
    Screen screen;
    struct stat info;
    if (stat(Game, &info) != 0)
        fail(screen, std::string(Game) + " is missing.");
    if (enabled)
        configure(screen, configPath, text, state);
    else
        pf::logInfo("launcher: start-up screen off (show_configurator_on_boot=off)");
    pf::logInfo("launcher: starting %s at %llu us", Game, (unsigned long long)sceKernelGetProcessTimeWide());
    const int result = sceAppMgrLoadExec(Game, nullptr, nullptr);
    while (result >= 0)
    {
        // Vita3K returns and switches to the game a moment later; the Vita does not return at all.
        sceKernelDelayThread(100000);
    }
    char line[64];
    snprintf(line, sizeof(line), "sceAppMgrLoadExec returned 0x%08x.", unsigned(result));
    fail(screen, line);
}
