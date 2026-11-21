// Start-up of the recompiled SPEED2.EXE: the folders, the checked executable loaded into the guest arena, the
// display, the guest registry and file system, then the game's entry point.

#include "audio_cull.h"
#include "audio_defer.h"
#include "audio_mixer.h"
#include "audio_split.h"
#include "audio_stats.h"
#include "bankcode.h"
#include "boot.h"
#include "mods_hook.h"
#include "notice.h"
#include "movies.h"
#include "mods_runtime.h"
#include "music_decode.h"
#include "default_cfg.h"
#include "frontend.h"
#include "hitch.h"
#include "textures.h"
#include <lib/memlog.h>
#include "offload.h"
#ifdef DEV_TOOLS
#include "../dev/record_test.h"
#endif
#include "scenery.h"
#include "padbind.h"
#include "textentry.h"

#include <lib/file.h>
#include <lib/memmap.h>
#include <recomp.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/crashlog.h>
#include <platform/cores.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/limiter.h>
#include <platform/perf.h>
#include <platform/sampler.h>
#include <platform/stall.h>
#ifdef NFSU_PGO_GENERATE
#include <platform/pgo.h>
#endif
#include <platform/sha256.h>
#include <fpu.h>
#include <mmx.h>
#include <ug2.h>
#include <ug2_layout.h>
#include <winapi/d3d9_native.h>
#include <winapi/kernel32.h>
#ifdef __vita__
#include "../../tools/shadercomp/shadercomp.h"
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#else
#include <host/crash.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__vita__) && defined(NFSU_BOOT_PROBE)
#include <psp2/io/fcntl.h>
static void bootMark(const char* path)
{
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0)
        sceIoClose(fd);
}
__attribute__((constructor(101))) static void bootProbeFirst()
{
    bootMark("ux0:data/NeedForSpeedUnderground2/probe_static.txt");
}
#endif

#ifdef __vita__
// Process parameters picked up by the VitaSDK runtime.
extern "C" {
int          _newlib_heap_size_user      = 64 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize  = 4 * 1024 * 1024;
}
#endif

namespace
{

const char* const ExeName = "SPEED2.EXE";

// Guest view of the executable and its folder.
const char* const GuestGameFolder = "C:\\";
const char* const GuestExePath = "C:\\SPEED2.EXE";
const char* const CommandLine = "\"C:\\SPEED2.EXE\"";

struct Folders
{
    std::string game;       // the game's files: SPEED2.EXE and its data folders
    std::string data;       // what the port and the game write
    std::string config;
    std::string log;
};

std::string join(const std::string& folder, const char* name)
{
    return folder + "/" + name;
}

#ifdef __vita__
// The data folder is the game folder and the guest's C: drive.
Folders folders(int, char**)
{
    Folders f;
    f.game = f.data = "ux0:data/NeedForSpeedUnderground2";
    f.config = join(f.game, "NeedForSpeedUnderground2.cfg");
    f.log = join(f.game, "NeedForSpeedUnderground2.log");
    mkdir("ux0:data", 0777);
    mkdir(f.game.c_str(), 0777);
    return f;
}
#else
std::string environment(const char* name, const std::string& fallback)
{
    const char* value = getenv(name);
    return value && *value ? std::string(value) : fallback;
}

void makeFolders(const std::string& path)
{
    for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
        mkdir(path.substr(0, slash).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

// The game folder stays read-only: the guest's writes go to the data folder.
Folders folders(int argc, char** argv)
{
    Folders f;
    f.game = environment("NFSU2_GAME_DIR", "");
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc)
            f.game = argv[++i];
        else if (!strncmp(argv[i], "--game-dir=", 11))
            f.game = argv[i] + 11;
    }
    while (f.game.size() > 1 && f.game.back() == '/')
        f.game.pop_back();
    f.data = environment("NFSU2_DATA_DIR", "ug2host-data");
    makeFolders(f.data);
    char resolved[PATH_MAX];
    if (realpath(f.data.c_str(), resolved))
        f.data = resolved;
    f.config = environment("NFSU2_CONFIG", join(f.game, "NeedForSpeedUnderground2.cfg"));
    f.log = environment("NFSU2_LOG", "ug2host.log");
    return f;
}
#endif

// Reads the executable; only the host build checks it.
bool loadExecutable(const std::string& folder, std::vector<uint8_t>& image)
{
    const std::string path = join(folder, ExeName);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
    {
        pf::logError("%s not found: copy the game folder's contents to %s/", path.c_str(), folder.c_str());
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    image.resize(size > 0 ? size_t(size) : 0);
    const size_t read = image.empty() ? 0 : fread(image.data(), 1, image.size(), file);
    fclose(file);
    if (read != image.size() || image.empty())
    {
        pf::logError("%s: reading %u bytes failed", path.c_str(), unsigned(image.size()));
        return false;
    }
#ifdef __vita__
    // The sections are mapped by file offset: a shorter file is padded so that never reads past its end.
    if (image.size() < ug2::layout::exeSize)
        image.resize(ug2::layout::exeSize);
    pf::logInfo("%s: %u bytes", path.c_str(), unsigned(size));
    return true;
#else
    const std::string hash = pf::sha256Hex(image.data(), image.size());
    if (image.size() != ug2::layout::exeSize)
    {
        pf::logError("%s: %u bytes, SHA-256 %s; the code was generated from %u bytes, SHA-256 %s, and a file of "
                     "another size cannot be mapped", path.c_str(), unsigned(image.size()), hash.c_str(),
                     unsigned(ug2::layout::exeSize), ug2::layout::exeSha256);
        return false;
    }
    if (hash != ug2::layout::exeSha256)
    {
        pf::logError("Hash check against SPEED2.EXE failed. Ensure that this is the original SPEED2.EXE version 1.2. "
                     "Modified versions may not work at all or result in crashes and performance issues in game. "
                     "(%s: SHA-256 %s, expected %s)", path.c_str(), hash.c_str(), ug2::layout::exeSha256);
        return true;
    }
    pf::logInfo("%s: %u bytes, SHA-256 %s", path.c_str(), unsigned(image.size()), hash.c_str());
    return true;
#endif
}

// Headers and section data at their guest addresses, zero-filled to size.
std::vector<win32::Section> imageSections(const std::vector<uint8_t>& image)
{
    std::vector<win32::Section> sections;
    sections.push_back(win32::Section{ image.data(), ug2::layout::imageBase, ug2::layout::headerSize });
    for (const ug2::layout::ImageSection& s : ug2::layout::sections)
    {
        const x86::reg32 size = std::max(s.virtualSize, s.rawSize);
        sections.push_back(win32::Section{ image.data() + s.fileOffset, s.address, std::min(s.rawSize, size) });
        sections.push_back(win32::Section{ nullptr, s.address, size });
    }
    return sections;
}

}

int main(int argc, char** argv)
{
#ifdef __vita__
#ifdef NFSU_BOOT_PROBE
    bootMark("ux0:data/NeedForSpeedUnderground2/probe_main.txt");
#endif
    // A shader compile request (tools/shadercomp/vita_compile.py) runs instead of the game.
    if (shadercomp::runIfRequested("ux0:data/NeedForSpeedUnderground2/shaders"))
        sceKernelExitProcess(0);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif
    const Folders f = folders(argc, argv);
#ifdef __vita__
    pf::crashlog::begin(f.data.c_str(), f.log.c_str(), NFSU_VERSION);
#endif
    pf::logInit(f.log.c_str());
#ifndef __vita__
    pf::crash::install();
#endif
    pf::logInfo("NeedForSpeedUnderground2 %s starting: game folder %s, data folder %s", NFSU_VERSION, f.game.c_str(), f.data.c_str());
#ifdef __vita__
    pf::logInfo("clocks: arm %d MHz, bus %d MHz, gpu %d MHz, xbar %d MHz", scePowerGetArmClockFrequency(),
                scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
#endif
    x86::mmxSelfTest();
    x86::fpuSelfTest();
#ifdef __vita__
    const bool cfgRead = pf::Config::load(f.config.c_str(), ug2::DefaultSettingsFile);
#else
    const bool cfgRead = pf::Config::load(f.config.c_str());
#endif
#ifdef __vita__
    pf::logSetConsole(pf::Config::getBool("console_log", false));
#else
    pf::logSetConsole(pf::Config::getBool("console_log", true));
#endif
#ifdef __vita__
    pf::logSetInfo(pf::Config::getBool("log_info", false));
#else
    pf::logSetInfo(pf::Config::getBool("log_info", true));
#endif
    pf::logSetDeferred(pf::Config::getBool("log_async", true));
#ifdef __vita__
    pf::crashlog::install();
#endif
    std::vector<std::string> notices = ug2::notice::collect(cfgRead, f.config, ug2::readModSettings(f.game));
#ifdef __vita__
    notices.insert(notices.end(), pf::crashlog::notices().begin(), pf::crashlog::notices().end());
#endif
    if (!notices.empty())
    {
        for (const std::string& line : notices)
            pf::logError("%s", line.c_str());
        ug2::notice::show(notices);
    }
    rc::g_promoteLo = x86::reg32(pf::Config::getInt("promote_lo", 0));
    rc::g_promoteSpan = x86::reg32(pf::Config::getInt("promote_hi", -1)) - rc::g_promoteLo;
#ifdef DEV_SETTINGS
    pf::logInfo("build switches: native functions %d, audio fast paths %d, render fast paths %d, each key can be set "
                "in the settings file (DEV_SETTINGS)", NATIVE_FUNCTIONS, AUDIO_FAST_PATHS, RENDER_FAST_PATHS);
#else
    pf::logInfo("build switches: native functions %d, audio fast paths %d, render fast paths %d, fixed in this build",
                NATIVE_FUNCTIONS, AUDIO_FAST_PATHS, RENDER_FAST_PATHS);
#endif
    pf::cores::init();
    pf::perf::init(pf::Config::getBool("timing_log", false), ug2::hitch::enabled());
    pf::limiter::init();
#ifdef NFSU_PGO_GENERATE
    pf::pgo::start(join(f.data, pf::pgo::FileName).c_str());
#endif
    pf::sampler::start(join(f.data, "samples.bin").c_str());
    pf::stall::start();
#ifdef __vita__
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#endif

    std::vector<uint8_t> image;
    if (!loadExecutable(f.game, image))
        pf::fatal("%s is missing or not the build the code was generated from (see the log)", ExeName);
    std::vector<win32::Section> sections = imageSections(image);
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");
    if (pf::Config::getBool("gxm_program_list", true))
        pf::gxm::Device::get().usePrograms(join(f.data, "gxm_programs.bin").c_str());

    win32::File::setDataDirectory((f.data + "/").c_str());
    win32::File::setCdDirectory((f.game + "/").c_str());
    win32::registry::load(join(f.data, "registry.txt").c_str());
    win32::kernel32::setModulePath(GuestExePath);
    win32::kernel32::setResourceSection(ug2::layout::imageBase, ug2::layout::resourceSection);
    ug2::bankcode::install();
    pf::logInfo("ug2: not ported yet: button prompts, "
                "memory pool report, translated bank routines");
    {
        ug2::Application app(CommandLine, sections);
        std::vector<win32::Section>().swap(sections);
        std::vector<uint8_t>().swap(image);
        ug2::modrt::applyPatches(win32::g_guestBase);
        ug2::mixer::configure(app);
        ug2::cull::configure(app);
        ug2::mixstats::configure(app);
        ug2::split::configure(app);
        ug2::sndq::configure(app);
        ug2::music::configure(app);
        ug2::boot::configure(app);
        ug2::textentry::configure(app);
        ug2::padbind::configure(app);
        ug2::hitch::configure(app);
        ug2::textures::configure(app);
        win32::memlog::configure(ug2::frontend::frameCounter);
        ug2::scenery::configure();
        ug2::offload::configure();
#ifdef DEV_TOOLS
        if (pf::Config::getBool("d3d_record_test", false))
            ug2::dev::recordTest();
#endif
        ug2::movies::configure(app);
        if (!win32::File::setCurrentDirectory(GuestGameFolder))
            pf::fatal("%s (the game folder) is missing", GuestGameFolder);
        app.execute();
        ug2::sndq::shutdown();
        // The deferred device calls finish before the application goes.
        win32::d3d9::bridge::drain();
    }
    pf::logInfo("NeedForSpeedUnderground2 exiting");
#ifdef __vita__
    pf::crashlog::end();
#endif
    pf::stall::stop();
    pf::sampler::stop();
#ifdef NFSU_PGO_GENERATE
    pf::pgo::stop();
    pf::pgo::write();
#endif
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
#ifdef __vita__
    sceKernelExitProcess(0);
#else
    // As on the Vita: no static destructors while job workers and guest threads still run.
    fflush(nullptr);
    _exit(0);
#endif
    return 0;
}
