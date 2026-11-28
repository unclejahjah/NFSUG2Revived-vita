// The table of every cfg key. The loader's known-key check, the configurator and the host tests read it.

#include <platform/settings.h>
#include <cstring>
#include <strings.h>

namespace pf
{

namespace
{

using T = SettingType;
using G = SettingGroup;
const unsigned DS = SettingNeedsDevSettings, DT = SettingNeedsDevTools, OFF = SettingOff, AUTO = SettingAuto,
               HOST = SettingHostOnly, SHADOWBUILD = SettingNeedsShadowBuild,
               SECRET = SettingSecret;

// Every Developer setting is read only in a DEV_SETTINGS build; a release build ignores it.
#define DEVONLY(group) ((group) == G::Developer ? SettingNeedsDevSettings : 0u)
#define BOOL(key, grp, def, desc, flags) { key, T::Bool, G::grp, def, 0, 1, 1, nullptr, desc, flags | DEVONLY(G::grp) }
#define INT(key, grp, def, lo, hi, step, desc, flags) { key, T::Int, G::grp, def, lo, hi, step, nullptr, desc, flags | DEVONLY(G::grp) }
#define ENUM(key, grp, def, names, desc, flags) { key, T::Enum, G::grp, def, 0, 0, 0, names, desc, flags | DEVONLY(G::grp) }
#define STR(key, grp, def, desc, flags) { key, T::String, G::grp, 0, 0, 0, 0, def, desc, flags | DEVONLY(G::grp) }
#define NATIVE(key, what) BOOL(key, Developer, 1, "Native version of " what "; off runs the translated original.", DS)
#define NATIVE_ALWAYS(key, what) BOOL(key, Developer, 1, "Native version of " what "; off runs the translated original.", 0)
#define FAST(key, what) BOOL(key, Developer, 1, "Fast path for " what "; off uses the plain one.", DS)

const Setting s_table[] =
{
    // ---- General ----
    BOOL("show_configurator_on_boot", General, 1, "Show the start-up screen (press START to configure) before the game boots.", 0),
    ENUM("language", General, 0, "default|french|german|italian|spanish|dutch|swedish|danish|korean|chinese|japanese", "The game's language, kept in registry.txt; default is the game's own. Missing files fall back to it.", SettingRegistry),
    BOOL("log_info", General, 0, "Write a detailed log to NeedForSpeedUnderground2.log. Errors are logged either way.", 0),
    BOOL("fast_travel", General, 1, "On the World Map, No on the GPS question puts the car at the chosen place (NFSU2FastTravel).", 0),
    BOOL("options_menu", General, 1, "The Vita Settings icon in the game's menus (main menu and Options), and Select + Up opens it.", 0),

    // ---- General: network play (src/lib/winapi/ws2_32.cpp, src/lib/winapi/revived) ----
    ENUM("network", General, 0, "revived|direct|off", "LAN and Online through the NFSU2 Revived server, over local Wi-Fi only, or offline.", 0),
    STR("revived_server", General, "nfsu2.xyz", "The NFSU2 Revived server (network=revived).", 0),
    STR("revived_name", General, "", "Name other players see on the Revived server; empty uses the Vita's user name.", 0),
    STR("revived_passphrase", General, "", "Your nfsu2.xyz passphrase, to play under the name it claimed. Never shown or logged.", SECRET),
    STR("online_server", General, "eahub.eu", "network=direct: the community server the Online menu uses, or off.", 0),

    // ---- General: crash and freeze reports (src/platform/crashlog.cpp, src/platform/stall.cpp) ----
    BOOL("crash_report", General, 1, "Write a crash report to the crashes folder when the game crashes (needs kubridge.skprx for registers).", 0),
    INT("stall_seconds", General, 8, 1, 600, 1, "Seconds without a frame before a stall is reported.", 0),

    // ---- Display ----
    ENUM("frame_rate", Display, 0, "30|60|0", "30 locks to 30 fps for steady pacing; 60 shows each frame for one vblank; 0 unlocks.", 0),
    ENUM("skip_intro_movies", Display, 0, "off|logos|opening", "Skip the start-up movies: off plays them; logos skips EA, THX and the notice; opening skips the opening movie too.", 0),
    ENUM("movies", Display, 0, "vp6preferred|vp6only", "vp6preferred: converted .mp4 if present, else native VP6; vp6only: always native VP6.", 0),

    // ---- Controls ----
    BOOL("rear_touch", Controls, 1, "Rear touch pad works as L2, R2, L3 and R3, unbound until set in the Controls screen.", SettingInGame),
    BOOL("garage_stick", Controls, 1, "The right stick turns the car in the garage and the customisation menus.", SettingInGame),
    INT("garage_stick_deadzone", Controls, 30, 0, 70, 1, "Right stick dead zone in the garage and customisation menus, in percent; raise it if the car spins alone.", 0),
    INT("free_look_deadzone", Controls, FreeLookDeadZoneDefault, 0, 70, 1, "Dead zone of the right stick for free look, in percent; raise it if the camera moves by itself.", 0),
    INT("stick_cone_angle", Controls, 25, 0, 45, 1, "Steer/throttle stick: degrees off its axis that still count as a full push. 0 is the PC game's way.", 0),
    BOOL("camera_stick", Controls, 1, "While driving the right stick turns the chase camera round the car (hood, bumper: down looks back); off: 0.9.0's free look.", 0),
    INT("camera_stick_speed", Controls, 4, 1, 10, 1, "How fast the right stick camera turns to the stick and comes back: 1 slowest, 10 fastest.", 0),
    BOOL("free_look_ease_back", Controls, 1, "Free look: let go less than 120 degrees round and the camera eases back behind the car; further round it cuts back.", 0),
    ENUM("horn_button", Controls, 4, "dpad_left|dpad_right|rear_l3|rear_r3|off|dpad_up|dpad_down|triangle|circle|square|cross|l|r|rear_l2|rear_r2", "Free roam's horn: hold this to honk (off by default: the HORN touch button above the N2O meter); the other players hear it. A short D-pad press is still the game's; a face button or L/R keeps its game action too.", 0),
    BOOL("horn_touch_button", Controls, 1, "Free roam's red horn button on the screen, above the N2O meter: touch and hold it to honk. Off: hidden, and touches there do nothing.", 0),
    ENUM("drift_button", Controls, 0, "dpad_right|dpad_left|rear_r3|rear_l3|off|dpad_up|dpad_down|triangle|circle|square|cross|l|r|rear_l2|rear_r2", "Free roam's drift mode: hold this to switch it on or off. A short D-pad press is still the game's; a face button or L/R keeps its game action too.", 0),
    ENUM("quick_chat_button", Controls, 2, "dpad_down|dpad_up|triangle|select", "Free roam's quick chat: hold this and press a phrase button (Vita Settings, Social); select: Select + the button.", 0),
    ENUM("steering", Controls, 0, "analog|digital", "analog: the left stick steers as far as it is pushed; digital: pushed past halfway it ramps to full lock, as a keyboard.", 0),

    // ---- Audio ----
    BOOL("music_hw_decode", Audio, 1, "Decode music with the Vita's MP3 decoder, which frees the CPU; the game's decoder is the fallback.", 0),
    ENUM("audio_mix_rate", Audio, 0, "0|22050|24000|32000|44100|48000", "Sound mixing rate in Hz; 0 is the game's (44100). 32000 needs less CPU, 22050 sounds duller.", 0),
    BOOL("audio_mixer_split", Audio, 1, "The sound mixer holds the game's sound lock for a shorter time, so the game waits less.", 0),
    INT("audio_silence_db", Audio, -60, -90, 0, 5, "Sounds quieter than this (dB below full scale) are not mixed, saving CPU; off mixes all.", OFF),
    INT("audio_block_ms", Audio, 20, 5, 40, 1, "Length of one sound output block in ms; shorter lowers latency and costs more CPU.", 0),

    // ---- Performance ----
    BOOL("optimised_particles", Performance, 1, "Cheaper tyre smoke and particles, close to the original picture; off is as the original.", 0),
    INT("dev_world_lod_pixels", Performance, 64, 0, 100000, 16, "Scenery narrower than this many pixels uses its simpler model. 0 is off; 64 by default.", 0),
    INT("dev_world_lod_min_tris", Performance, 48, 0, 100000, 4, "Models with fewer triangles keep their full detail under dev_world_lod_pixels.", 0),
    INT("dev_world_lod_max_percent", Performance, 75, 0, 100, 5, "Most of a model's triangles its simpler model may keep, in percent.", 0),
    INT("dev_world_lod_min_width", Performance, 10, 0, 10000, 10, "Scenery types narrower than this many metres keep their full model.", 0),
    BOOL("dev_world_lod_signs", Performance, 0, "Signs (XS_ models) also use the simpler model; off keeps every sign at full detail.", 0),
    INT("dev_world_min_pixels", Performance, 12, 0, 64, 1, "Scenery smaller than this many pixels is not drawn. 0 is off, 64 the most; 12 by default; more pop-in.", 0),
    BOOL("dev_world_min_pixels_lights", Performance, 1, "Models with light markers are never culled by dev_world_min_pixels; off culls them too.", 0),
    INT("dev_reflection_min_pixels", Performance, 0, 0, 64, 1, "dev_world_min_pixels for the reflection and other views; auto uses the world value.", AUTO),
    BOOL("use_core3", Performance, 1, "Use the Vita's third CPU core when it can be unlocked.", 0),
    ENUM("core3_tasks", Performance, 0, "helpers|jobs", "Work on the third core: helper and io threads with the command-list worker, or job workers.", 0),
    INT("texture_cache_mb", Performance, 12, 0, 64, 1, "Memory for kept texture levels in MB; more avoids reloading them.", 0),
    INT("texture_heap_mb", Performance, 0, 0, 1024, 16, "Largest size of the texture heap in MB; 0 is no cap.", 0),

    // ---- Developer: boot, front end ----
    BOOL("ui_log", Developer, 0, "Log the user interface positions the game sets.", 0),
    BOOL("ps2_prompts", Developer, 1, "PS2-style prompts: button icons in every menu text and Help on Triangle; off is the PC menus.", 0),
    BOOL("pad_log", Developer, 0, "Log the pad bindings and the buttons pressed.", 0),
    INT("free_look_swing", Controls, 180, 0, 2000, 10, "Free look: degrees a second the camera swings to aims less than 90 from ahead; 0 snaps.", SettingCfgOnly),
    BOOL("free_look_release_swing", Controls, 0, "Free look: letting go swings back to the chase view instead of snapping.", SettingCfgOnly),
    STR("pad_script", Developer, "", "Scripted pad input for tests.", 0),
    BOOL("boot_race", Developer, 0, "Skip the menus and start a test race.", 0),
    ENUM("boot_mode", Developer, 0, "race|url|drag|drift|freeroam", "Kind of race boot_race starts.", 0),
    INT("boot_track", Developer, 0, 0, 9999, 1, "Track number of the boot race; auto picks one for the mode.", AUTO),
    STR("boot_car", Developer, "", "Car of the boot race; empty keeps the game's choice.", 0),
    INT("boot_laps", Developer, 2, 1, 10, 1, "Laps of the boot race.", 0),
    BOOL("boot_ai_driver", Developer, 1, "An AI drives the player's car in the boot race.", 0),
    BOOL("boot_seed", Developer, 1, "Fixed random seed in the boot race.", 0),
    BOOL("boot_hud", Developer, 0, "Show the HUD in the boot race.", 0),
    BOOL("boot_audio", Developer, 1, "Sound in the boot race; auto is off with virtual_clock, on otherwise.", AUTO),
    BOOL("boot_crowd", Developer, 0, "Crowds in the boot race.", 0),
    BOOL("boot_intro", Developer, 0, "Play the pre-race intro in the boot race.", 0),
    BOOL("boot_traffic", Developer, 0, "Traffic in the boot race (old form of boot_traffic_level).", 0),
    INT("boot_traffic_level", Developer, 0, 0, 3, 1, "Traffic density in the boot race; auto follows boot_traffic.", AUTO),
    INT("boot_opponents", Developer, 0, 0, 5, 1, "Opponents in the boot race; auto depends on the mode.", AUTO),
    BOOL("boot_power_tick", Developer, 1, "Keep the system from idling during a benchmark.", DT),
    BOOL("virtual_clock", Developer, 0, "Run the game on a virtual clock for repeatable test runs.", 0),
    BOOL("boot_drive", Developer, 1, "Scripted blind driving in the free-roam boot.", DT),
    INT("boot_drive_seed", Developer, 1, 0, 9999, 1, "Seed of the scripted driving.", DT),
    BOOL("boot_manual", Developer, 1, "Manual gearbox in the free-roam and drag boot.", 0),
    INT("boot_upgrades", Developer, 2, 0, 3, 1, "Upgrade level of the car in the free-roam boot.", 0),
    STR("boot_script", Developer, "", "Scripted pad input and front-end log for host runs.", 0),
    STR("boot_steps", Developer, "", "Scripted button steps for the front end.", 0),
    STR("boot_quit_after", Developer, "", "Quit after N seconds, stateN+frames or save+frames.", 0),
    BOOL("fe_log", Developer, 1, "Log the front-end state changes.", 0),
    BOOL("fe_files", Developer, 1, "Log the files the front end opens.", 0),
    BOOL("fe_msg", Developer, 0, "Log the front-end messages.", 0),
    BOOL("fe_orbit", Developer, 0, "Log the garage camera yaw.", 0),
    BOOL("fe_dump", Developer, 0, "Dump front-end data to the log.", 0),
    BOOL("fe_grep_words", Developer, 0, "Match whole words in fe_grep.", 0),
    STR("fe_grep", Developer, "", "Log guest memory strings that contain this text.", 0),
    INT("fe_grep_frame", Developer, 0, 0, 100000, 100, "Frame at which fe_grep runs.", 0),
    STR("fe_scan_file", Developer, "", "Log strings of this file found twice or more in guest memory.", 0),
    INT("fe_scan_every", Developer, 0, 0, 100000, 100, "Frames between fe_scan_file scans.", 0),
    STR("fe_gfx_apply", Developer, "", "A live Display menu change: name:value,...", 0),
    INT("fe_gfx_apply_delay", Developer, 300, 0, 10000, 100, "Frames before fe_gfx_apply is applied.", 0),
    STR("movies_skip", Display, "0", "Ends each movie after this many seconds; 0 plays them whole.", SettingCfgOnly),
    STR("movies_dir", Display, "", "Folder of the converted movies; empty uses the default.", SettingCfgOnly),
    STR("movies_selftest", Developer, "", "Self-test of the movie decoder on this file.", 0),
    STR("movies_vp6_selftest", Developer, "", "Self-test of the native VP6 decoder on this file.", 0),
    BOOL("movies_vp6_worker", Developer, 1, "Decode VP6 movies ahead on a worker thread; off decodes on the game thread.", DS),
    INT("movies_vp6_queue", Developer, 6, 1, 15, 1, "VP6 frames decoded ahead.", DS),
    ENUM("movies_vp6_split", Developer, 1, "off|on|core0", "VP6 coefficients on a second thread: off, on (core 3 only) or on core 0.", DS),
    BOOL("movies_vp6_unlock", Developer, 0, "Decode VP6 movies without the execution lock.", DS),
    STR("movies_game_dir", Developer, "", "Folder below the data folder the game opens its .vp6 movies from (with movies_dir).", DS),
    STR("movies_redirect", Developer, "", "from:to, the game opens to.vp6 in place of from.vp6.", DS),
    STR("ime_test", Developer, "", "Text the keyboard test types.", 0),
    STR("ime_test_field", Developer, "", "Field the keyboard test fills.", 0),
    INT("gfx_level", Developer, 0, 0, 5, 1, "Measurement level of the vita graphics preset; 0 uses the preset itself.", 0),
    STR("gfx_set", Developer, "", "Graphics overrides on top of the preset: name:value,...", 0),
    INT("promote_lo", Developer, 0, 0, 0x7fffffff, 1, "First function whose slots stay in locals (bisecting a generator option).", 0),
    INT("promote_hi", Developer, -1, -1, 0x7fffffff, 1, "Last function whose slots stay in locals; -1 is no limit.", 0),

    // ---- Developer: logs, timing, stalls ----
    BOOL("console_log", Developer, 0, "Also print the log to the console.", 0),
    BOOL("log_async", Developer, 1, "Write the log on another thread.", 0),
    BOOL("memory_log", Developer, 0, "Log memory use.", 0),
    INT("memory_log_seconds", Developer, 10, 1, 600, 1, "Seconds between memory log lines.", 0),
    BOOL("timing_log", General, 0, "Log frame timing.", SettingCfgOnly),
    INT("timing_interval", General, 5000, 250, 60000, 250, "Milliseconds between timing log lines.", SettingCfgOnly),
    BOOL("hitch_log", Developer, 0, "Log frames that take much longer than the average.", DT),
    BOOL("scene_log", Developer, 0, "Log the scenery drawn.", DT),
    INT("scene_log_frames", Developer, 0, 0, 100000, 10, "Frames between scene log reports; 0 reports every frame.", DT),
    BOOL("dev_mixer_stats", Developer, 0, "Count the mixer's work and report it.", DT),
    BOOL("dev_cull_stats", Developer, 0, "Count the silent voices skipped and report it.", 0),
    BOOL("dev_snd_watch", Developer, 0, "Watch how long the game's sound lock is held.", 0),
    BOOL("stall_report", Developer, 1, "Report when the game stops presenting frames.", 0),
    BOOL("stall_direct", Developer, 0, "Stall reports bypass the log lock.", 0),
    INT("stall_heartbeat", Developer, 0, 0, 600, 1, "Seconds between heartbeat lines; 0 is off.", 0),
    INT("stall_priority", Developer, 0, 0, 255, 1, "Priority of the stall watcher thread.", AUTO),
    INT("stall_crash", Developer, 0, 0, 600, 1, "Crash for a dump after this many seconds without a frame; 0 is off.", 0),
    INT("stall_cpu", Developer, 0, 0, 15, 1, "CPU mask of the stall watcher thread; 0 is any.", 0),
    BOOL("stall_power_tick", Developer, 0, "Keep the system from idling into standby.", 0),
    BOOL("limiter_log", Developer, 0, "Log the frame limiter's timing.", 0),
    BOOL("wait_probe", Developer, 0, "Log long waits in the Windows wait functions.", 0),
    BOOL("sampler", General, 0, "Record a sampling profile.", SettingCfgOnly),
    INT("sampler_interval_us", General, 1000, 200, 100000, 100, "Microseconds between samples.", SettingCfgOnly),
    BOOL("sampler_copies", General, 0, "Copy the sampled stacks.", SettingCfgOnly),
    ENUM("sampler_thread", General, 0, "game|mixer|io", "Thread the sampler watches.", SettingCfgOnly),
    BOOL("sampler_from_race", General, 0, "Start sampling when a race starts.", SettingCfgOnly),

    // ---- Developer: benchmark ----
    BOOL("benchmark_exit", Developer, 0, "Quit when the benchmark ends.", DT),
    INT("benchmark_seconds", Developer, 0, 0, 3600, 5, "Length of the benchmark in seconds; auto is the boot's own length.", AUTO | DT),
    INT("benchmark_frames", Developer, 0, 0, 1000000, 100, "Length of the benchmark in frames; 0 is no limit.", DT),
    INT("benchmark_window", Developer, 0, 0, 3600, 5, "Seconds the benchmark measures; auto is the boot's own window.", AUTO | DT),

    // ---- Developer: renderer ----
    ENUM("alpha_draw_order", Developer, 0, "sorted|game", "Order of transparent draws: sorted, or as the game submits them.", 0),
    FAST("skip_unused_alpha_test", "dropping an alpha test the shader does not use"),
    BOOL("texture_async", Developer, 1, "Create textures on a worker thread.", 0),
    BOOL("texture_batch", Developer, 1, "Create textures in batches.", 0),
    BOOL("texture_instant_free", Developer, 1, "Free textures at once instead of at the end of the frame.", 0),
    BOOL("texture_retry", Developer, 1, "Create a failed texture again and draw it black meanwhile.", 0),
    BOOL("texture_release_all", Developer, 1, "Empty the game's texture release queue every frame.", 0),
    BOOL("texture_load_drain", Developer, 1, "Finish texture work before a world load.", 0),
    BOOL("texture_heap_shrink", Developer, 1, "Give unused texture heap back.", 0),
    BOOL("guest_memory_split", Developer, 1, "Split the game's memory into separate blocks.", 0),
    FAST("gxm_precomputed", "precomputed GXM states and draws"),
    BOOL("gxm_lists", Developer, 0, "Record GXM command lists on worker threads.", 0),
    BOOL("gxm_core2", Developer, 1, "Allow a command list worker on core 2.", 0),
    BOOL("gxm_program_list", Developer, 1, "Load the list of shader programs to build at start.", 0),
    BOOL("cull_swap", Developer, 0, "Swap the culling direction.", 0),
    STR("debug_instruction", Developer, "", "wait: Customize's instruction line shows the bind wait text.", DS),
    BOOL("debug_frame_dump", Developer, 0, "Dump frames to files.", 0),
    INT("debug_hold_frame", Developer, 0, 0, 100000, 100, "Hold the shown frame from this frame on and write hold.bmp; 0 is off.", DT),
    INT("debug_skip_vp", Developer, 0, 0, 1000, 1, "Skip drawing with this vertex program; 0 is off.", 0),
    INT("debug_skip_fp", Developer, 0, 0, 1000, 1, "Skip drawing with this fragment program; 0 is off.", 0),
    INT("debug_max_draws", Developer, 0, 0, 100000, 10, "Draw at most this many calls a frame; 0 is no limit.", 0),
    INT("debug_scene_scale", Developer, 100, 10, 100, 5, "Scale of each scene in percent, drawn in its target's corner.", 0),
    INT("debug_scene_dump", Developer, 0, 0, 100000, 1, "Log every Nth frame's scenes.", HOST),
    INT("debug_merge_limit", Developer, 1000, 0, 100000, 100, "Largest number of draws merged.", 0),
    INT("debug_plan_check", Developer, 0, 0, 100000, 1, "Every Nth frame is drawn as recorded and as planned, and compared.", 0),
    STR("debug_shot_frames", Developer, "", "Race frames to save as shot_N.bmp: a,b,c; fN rendered frame N, lN N frames after loading.", DT),
    INT("debug_control_type", Developer, 0, 0, 5, 1, "Plays with this Control Type (1 to 4, 5 Custom) without storing it; 0 uses registry.txt.", DS),
    BOOL("debug_draw_hash", Developer, 0, "Hash each draw.", 0),
    BOOL("debug_scene_finish", Developer, 0, "Wait for the GPU after each scene.", 0),
    FAST("scene_merge", "merging scenes"),
    BOOL("blend_op_exact", Developer, 1, "Reproduce blend operations exactly.", 0),
    FAST("copy_elide", "dropping copies of the same image"),
    FAST("dead_passes", "dropping passes whose result is not used"),
    BOOL("effect_stats", Developer, 0, "Count effect uses.", 0),
    INT("effect_trace", Developer, 0, 0, 100000, 10, "Trace this many effect calls.", 0),
    BOOL("effect_check", Developer, 0, "Check effects against the plain path.", 0),
    BOOL("smoke_log", Developer, 0, "Log the smoke settings.", DS),
    BOOL("dev_smoke_skip", Developer, 0, "Do not draw smoke.", DS),
    STR("dev_smoke_fade", Developer, "", "Smoke fade range in percent of the viewports: from,to.", DS),
    STR("dev_smoke_near", Developer, "", "Smoke near fade distances: from,to.", DS),
    ENUM("dev_smoke_mip", Developer, 0, "linear|point", "point samples the smoke mip levels with point filtering.", DS),
    BOOL("display_vblank_wait", Developer, 1, "Wait for the vertical blank when showing a frame.", 0),
    INT("host_gpu_latency", Developer, 1, 0, 8, 1, "Frames a submitted frame stays in flight.", HOST),
    STR("frame_stats", Developer, "", "File to write frame statistics to.", HOST),
    BOOL("host_check_ranges", Developer, 0, "Check ranges of the host copies.", HOST),
    FAST("d3d_fast_effects", "the Direct3D effect calls"),
    FAST("d3d_fast_draw", "draw calls"),
    FAST("d3d_fast_layer", "the Direct3D layer"),
    FAST("flush_direct", "flushing effect state"),
    BOOL("offload_flush", Developer, 1, "Draw flushes run as recorded segments; off flushes directly.", DS),
    BOOL("offload_context", Developer, 1, "Run each flush with its own context and device clone.", DS),
    BOOL("offload_sequential", Developer, 0, "Hand each flush to the worker and wait for it at once.", DS),
    BOOL("offload_poison", Developer, 0, "Poison a context's state after each flush to find leaks.", DS),
    ENUM("offload_unload_mode", Developer, 0, "cancel|fence|off", "Unloads cancel the queued draws that use what they free, or wait for every flush.", DS),
    INT("offload_unload_poison", Developer, 0, 0, 15, 1, "Test: poison unloaded memory; 1 solids, 2 texture packs, 4 streamed textures, 8 chunks.", DT),
    INT("offload_worker_delay", Developer, 0, 0, 1000, 1, "Test: the flush worker waits this many ms before each flush.", DS),
    BOOL("offload_count", Developer, 0, "Count the flush's reads and writes of shared state.", DT),
    BOOL("offload_probe", Developer, 0, "Count the flush's reads of state and simulate a late join.", DT),
    BOOL("debug_constants_zero", Developer, 0, "Clear constants no program writes, so draw hashes compare.", DT),
    INT("debug_draw_hash_from", Developer, -1, -1, 1000000, 1, "First frame whose draws are hashed; -1 is all.", 0),
    BOOL("d3d_record_test", Developer, 0, "Run the Direct3D recorder self-test at start.", DT),

    // ---- Developer: audio ----
    BOOL("audio_log", Developer, 0, "Log the audio output.", 0),
    ENUM("audio_mixer", Developer, 0, "unlocked|locked", "Mix without the game's execution lock (unlocked) or with it.", 0),
    BOOL("audio_defer", Developer, 1, "Defer sound work off the game thread.", 0),
    BOOL("audio_defer_log", Developer, 0, "Log the deferred sound work.", 0),
    BOOL("audio_defer_notify", Developer, 0, "Notify instead of polling for deferred sound work.", 0),
    BOOL("audio_defer_hook", Developer, 1, "Hook the sound tick for deferred work.", 0),
    BOOL("audio_inline_with_hooks", Developer, 0, "Run the sound engine inline even with hooks.", 0),
    BOOL("audio_hook_release", Developer, 1, "Release the sound hook early.", 0),
    INT("audio_hook_wait_ms", Developer, 10, 0, 1000, 1, "Milliseconds the sound hook waits.", 0),
    BOOL("audio_snd_yield", Developer, 0, "Yield the sound lock to the game.", 0),
    BOOL("audio_mixer_stay", Developer, 1, "Keep the mixer thread running between blocks.", 0),
    STR("audio_dump", Developer, "", "File to dump the mixed sound to.", 0),
    BOOL("audio_engine_inline", Developer, 1, "Run the sound engine inline.", 0),
    BOOL("ea_timer_unlocked", Developer, 1, "The EA timer runs without the execution lock.", 0),
    INT("yield_relock_spin_us", Developer, 0, 0, 10000, 10, "Microseconds to spin before taking the lock again.", 0),
    BOOL("audio_inline_notifications", Developer, 1, "Run sound notifications inline.", 0),
    BOOL("audio_library_audit", Developer, 1, "Check the sound library's calls.", 0),
    FAST("audio_native_kernels", "the mixer kernels"),
    FAST("audio_neon_kernels", "the mixer kernels in NEON"),
    BOOL("audio_selftest", Developer, 0, "Run the audio self-test at start.", 0),
    BOOL("audio_hash", Developer, 0, "Hash the mixed sound.", 0),
    BOOL("audio_cull_check", Developer, 0, "Check culled voices against the full mix.", 0),
    BOOL("audio_split_check", Developer, 0, "Check the split mixer against the original.", 0),
    BOOL("bank_cache_check", Developer, 0, "Check the sound bank cache.", 0),
    BOOL("bank_lib_check", Developer, 0, "Check the sound bank library.", 0),
    BOOL("bank_native", Developer, 1, "Native sound bank routines; off runs the translated ones.", DS),
    BOOL("shadow_bank", Developer, 0, "Also run the translated sound bank routines and compare.", 0),
    BOOL("music_hw_ahead", Developer, 1, "Decode the next music frame ahead on the worker.", 0),
    BOOL("music_hw_check", Developer, 0, "Compare the hardware music decoder with the game's.", 0),
    STR("music_hw_check_dump", Developer, "", "File to dump the music comparison to.", 0),

    // ---- Developer: native code ----
    BOOL("native_shadow", Developer, 0, "Compare native functions against their translated originals.", SHADOWBUILD),
    INT("native_shadow_rate", Developer, 8, 1, 1000, 1, "Compare every Nth call.", SHADOWBUILD),
    BOOL("phys_native_check", Developer, 0, "Check the native physics against the translated code.", 0),
    NATIVE("material_native", "material setup"),
    NATIVE_ALWAYS("matrix4_native", "4x4 matrix maths"),
    NATIVE("box_cull_native", "box culling"),
    BOOL("box_neon", Developer, 1, "Box culling in NEON.", DS),
    NATIVE("tree_cull_native", "tree culling"),
    NATIVE("draw_scenery_native", "scenery drawing"),
    NATIVE("world_upload_native", "the world upload"),
    NATIVE("texture_step_native", "the texture step"),
    NATIVE("light_upload_native", "the light upload"),
    NATIVE("flush_native", "the draw flush"),
    NATIVE("sub_5bc6f0_native", "sub_5bc6f0"),
    NATIVE("sub_5bc7b0_native", "sub_5bc7b0"),
    NATIVE("sub_5bc890_native", "sub_5bc890"),
    NATIVE("sub_5bcad0_native", "sub_5bcad0"),
    NATIVE("sub_5bcb10_native", "sub_5bcb10"),
    NATIVE("sub_5bdcd0_native", "sub_5bdcd0"),
    NATIVE("sub_4901d0_native", "sub_4901d0"),
    NATIVE("entry_resolve_native", "entry resolving"),
    NATIVE("ui_pos_native", "user interface positions"),
    NATIVE("sine_poly_native", "the sine polynomial"),
    NATIVE("point_in_quad_native", "the point in quad test"),
    NATIVE("vec_blend_native", "vector blending"),
    NATIVE("sub_61b670_native", "sub_61b670"),
    NATIVE("entry_pick_native", "entry picking"),
    NATIVE("mixer_accumulate_native", "mixer accumulation"),
    NATIVE("ground_query_native", "ground queries"),
    NATIVE("ug2_draw_walk_native", "the draw walk"),
    NATIVE("corner_walk_native", "the corner walk"),
    NATIVE("closest_feature_native", "closest feature"),
    NATIVE("view_adjust_native", "view adjustment"),
    NATIVE("chase_cam_native", "the chase camera"),
    NATIVE_ALWAYS("shadow_hull_native", "shadow hulls"),
    NATIVE_ALWAYS("particles_native", "particles"),
    NATIVE("particle_update_native", "particle updates"),
    NATIVE("particle_draw_native", "the particle draw pass"),
    NATIVE("headlight_pass_native", "the headlight pass"),
    NATIVE("scenery_stuff_native", "scenery helpers"),
    NATIVE_ALWAYS("shadow_points_native", "shadow points"),
    NATIVE_ALWAYS("vec_helpers_native", "vector helpers"),
    NATIVE("phys_vector_native", "physics vector maths"),
    NATIVE("drivetrain_native", "the drivetrain"),
    NATIVE("wheel_native", "wheels"),
    NATIVE("shape_bounds_native", "shape bounds"),
    NATIVE_ALWAYS("view_visible_native", "view visibility"),
    NATIVE("light_gather_native", "light gathering"),
    NATIVE_ALWAYS("light_flare_native", "light flares"),
    NATIVE_ALWAYS("hierarchy_native", "the scene node hierarchy"),
    NATIVE_ALWAYS("anim_update_native", "the animated object update"),
    NATIVE_ALWAYS("pose_native", "the pose build"),
    NATIVE_ALWAYS("anim_time_native", "the animation time index"),
    NATIVE_ALWAYS("anim_sample_native", "keyframe sampling"),
    NATIVE_ALWAYS("anim_quant_native", "quantised keyframe sampling"),
    NATIVE_ALWAYS("anim_quant3_native", "quantised three-channel keyframe sampling"),
    NATIVE("movies_convert_native", "the movie frame conversion"),

    // ---- Developer: save folder, files ----
    STR("save_dir", Developer, "", "Save folder override; empty uses the folder below the data folder.", 0),
    BOOL("save_log", Developer, 0, "Log the save file operations.", 0),
    BOOL("save_sync", Developer, 1, "Flush saves to the card at once.", 0),
    BOOL("save_atomic", Developer, 1, "Write saves to a temporary file and rename.", 0),
    BOOL("file_lazy_seek", Developer, 1, "Seek files only when needed.", 0),
    BOOL("file_handle_cache", Developer, 1, "Keep file handles open.", 0),
    BOOL("texture_merge_native", Developer, 1, "Merge texture pack tables natively (car loads).", 0),

};

#undef BOOL
#undef INT
#undef ENUM
#undef STR
#undef NATIVE
#undef NATIVE_ALWAYS
#undef FAST

// Keys that no longer exist: a cfg line for one is skipped without a message.
const char* const s_retired[] = { "front_touch", "widescreen", "control_type", "custom_binds", "left_stick_mode", "right_stick_mode",
                                  "dpad_mode", "free_look", "gfx_preset",
                                  "left_stick_deadzone", "left_stick_saturation", "left_stick_sensitivity", "right_stick_deadzone",
                                  "right_stick_saturation", "right_stick_sensitivity",
                                  // the fork's own, replaced by 0.9.0's stick rows (stick_deadzone and
                                  // stick_response fill in the stick rows once: src/ug2/controlslink.cpp)
                                  "stick_deadzone", "stick_response" };

const SettingAlias s_aliases[] =
{
    { "world_lod_pixels", "dev_world_lod_pixels" },
    { "world_lod_min_tris", "dev_world_lod_min_tris" },
    { "world_lod_max_percent", "dev_world_lod_max_percent" },
    { "world_lod_min_width", "dev_world_lod_min_width" },
    { "world_lod_signs", "dev_world_lod_signs" },
    { "world_min_pixels", "dev_world_min_pixels" },
    { "world_min_pixels_lights", "dev_world_min_pixels_lights" },
    { "reflection_min_pixels", "dev_reflection_min_pixels" },
    { "smoke_skip", "dev_smoke_skip" },
    { "smoke_fade", "dev_smoke_fade" },
    { "smoke_near", "dev_smoke_near" },
    { "smoke_mip", "dev_smoke_mip" },
};

}

const Setting* settingsTable(int* count)
{
    *count = int(sizeof(s_table) / sizeof(s_table[0]));
    return s_table;
}

const Setting* findSetting(const char* key)
{
    for (const Setting& setting : s_table)
    {
        if (strcmp(setting.key, key) == 0)
            return &setting;
    }
    return nullptr;
}

const char* settingGroupName(SettingGroup group)
{
    static const char* const Names[] = { "General", "Display", "Controls", "Audio", "Performance", "Developer" };
    return group < SettingGroup::Count ? Names[int(group)] : "";
}

bool settingReadInThisBuild(const Setting& setting)
{
    unsigned missing = 0;
#ifndef DEV_SETTINGS
    missing |= SettingNeedsDevSettings;
#endif
#ifndef DEV_TOOLS
    missing |= SettingNeedsDevTools;
#endif
#ifndef NFSU_HOST
    missing |= SettingHostOnly;
#endif
#ifndef NFSU_NATIVE_SHADOW
    missing |= SettingNeedsShadowBuild;
#endif
    return (setting.flags & missing) == 0;
}

bool settingRetired(const char* key)
{
    for (const char* retired : s_retired)
    {
        if (strcmp(retired, key) == 0)
            return true;
    }
    return false;
}

const SettingAlias* settingAliases(int* count)
{
    *count = int(sizeof(s_aliases) / sizeof(s_aliases[0]));
    return s_aliases;
}

const char* canonicalKey(const char* key)
{
    for (const SettingAlias& alias : s_aliases)
    {
        if (strcmp(alias.oldKey, key) == 0)
            return alias.key;
    }
    return key;
}

const char* oldKeyOf(const char* key)
{
    for (const SettingAlias& alias : s_aliases)
    {
        if (strcmp(alias.key, key) == 0)
            return alias.oldKey;
    }
    return nullptr;
}

int enumCount(const Setting& setting)
{
    if (setting.type != SettingType::Enum)
        return 0;
    int count = 1;
    for (const char* p = setting.text; *p; ++p)
        count += *p == '|';
    return count;
}

std::string enumName(const Setting& setting, int index)
{
    if (setting.type != SettingType::Enum || index < 0)
        return std::string();
    const char* p = setting.text;
    for (int i = 0; i < index && p; ++i)
    {
        p = strchr(p, '|');
        p = p ? p + 1 : nullptr;
    }
    if (!p)
        return std::string();
    const char* end = strchr(p, '|');
    return end ? std::string(p, end) : std::string(p);
}

int enumIndex(const Setting& setting, const char* name)
{
    const int count = enumCount(setting);
    for (int i = 0; i < count; ++i)
    {
        if (strcasecmp(enumName(setting, i).c_str(), name) == 0)
            return i;
    }
    return -1;
}

std::string defaultText(const Setting& setting)
{
    if (setting.flags & SettingAuto)
        return "auto";
    switch (setting.type)
    {
    case SettingType::Bool:
        return setting.def ? "on" : "off";
    case SettingType::Int:
        return std::to_string(setting.def);
    case SettingType::Enum:
        return enumName(setting, setting.def);
    case SettingType::String:
        return setting.text;
    }
    return std::string();
}

}
