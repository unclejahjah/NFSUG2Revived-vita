#include <platform/config.h>
#include <platform/log.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pf
{

namespace
{

const int MaxEntries = 64;
const int MaxKey     = 32;
const int MaxValue   = 384;
const int MaxLine    = 448;

// Every setting the game reads; other keys are reported as unknown.
const char* const Known[] =
{
    // Player settings.
    "widescreen", "ui_log", "button_prompts", "frame_rate", "car_reflection_interval", "rear_touch", "front_touch", "garage_stick", "garage_stick_deadzone", "garage_stick_speed", "pad_log", "pad_script",
    "music_hw_decode", "world_min_pixels", "reflection_min_pixels", "world_min_pixels_lights", "world_lod_pixels", "world_lod_min_tris", "world_lod_max_percent", "world_lod_min_width", "world_lod_signs", "scene_log", "scene_log_frames", "timing_interval", "material_native", "matrix4_native", "box_cull_native", "box_neon", "tree_cull_native", "draw_scenery_native",
    "world_upload_native", "texture_step_native", "light_upload_native", "flush_native",
    "entry_resolve_native", "ui_pos_native", "sine_poly_native", "point_in_quad_native", "vec_blend_native", "sub_61b670_native",
    "entry_pick_native", "mixer_accumulate_native", "ground_query_native", "ug2_draw_walk_native", "corner_walk_native",
    "closest_feature_native", "view_adjust_native", "chase_cam_native",
    "shadow_hull_native", "particles_native", "particle_update_native", "headlight_pass_native", "scenery_stuff_native",
    "shadow_points_native", "vec_helpers_native", "phys_vector_native", "drivetrain_native", "wheel_native", "shape_bounds_native", "phys_native_check", "view_visible_native", "light_gather_native", "light_flare_native",
    // Cores, logs and the benchmark.
    "use_core3", "core3_tasks", "timing_log", "timing_phases", "hitch_log", "console_log", "log_info", "memory_log", "memory_log_seconds", "log_async", "stall_report",
    "stall_seconds", "stall_direct", "stall_heartbeat", "stall_priority",
    "stall_crash", "stall_cpu", "stall_power_tick",
    "benchmark", "benchmark_exit", "benchmark_seconds", "benchmark_laps", "benchmark_opponents", "benchmark_track",
    "benchmark_timing_log", "benchmark_mode", "benchmark_car", "benchmark_driver", "benchmark_window", "benchmark_frames",
    // Direct boot into a race and the virtual clock (src/ug2/boot.cpp).
    "boot_race", "boot_track", "boot_car", "boot_laps", "boot_ai_driver", "boot_seed", "boot_hud", "boot_audio",
    "boot_crowd", "boot_traffic", "boot_traffic_level", "boot_opponents", "boot_power_tick", "virtual_clock", "boot_mode", "boot_drive_seed", "boot_drive", "boot_manual", "boot_upgrades",
    // Graphics preset (src/lib/registry.cpp).
    "gfx_preset", "gfx_set", "gfx_level",
    // Renderer.
    "alpha_draw_order", "skip_unused_alpha_test", "matrices_on_render_thread", "texture_cache_mb", "guest_memory_split", "texture_async", "texture_batch",
    "texture_instant_free", "texture_heap_mb", "texture_retry", "texture_release_all", "texture_load_drain", "texture_heap_shrink",
    "gxm_precomputed", "gxm_lists", "gxm_core2", "gxm_program_list", "cull_swap", "debug_frame_dump", "debug_hold_frame",
    "debug_skip_vp", "debug_skip_fp", "debug_max_draws", "debug_scene_scale", "debug_scene_dump", "debug_merge_limit", "debug_plan_check", "debug_shot_frames", "debug_draw_hash", "scene_merge", "blend_op_exact", "copy_elide", "dead_passes",
    "effect_stats", "effect_trace", "effect_check", "optimised_particles", "smoke_log", "smoke_skip", "smoke_fade", "smoke_near", "smoke_mip",
    // Native versions of guest code and their shadow compares.
    "audio_log", "audio_silence_db", "audio_cull_check", "dev_cull_stats", "audio_block_ms", "audio_mix_rate", "audio_mixer", "audio_defer", "audio_defer_log", "audio_defer_notify", "audio_defer_hook", "audio_inline_with_hooks", "audio_hook_release", "audio_hook_wait_ms", "audio_snd_yield", "audio_mixer_stay", "audio_dump", "audio_engine_inline", "ea_timer_unlocked", "yield_relock_spin_us", "audio_inline_notifications", "audio_library_audit", "audio_native_kernels", "audio_neon_kernels", "audio_selftest", "audio_hash", "bank_native", "shadow_bank", "bank_cache_check", "bank_lib_check", "coll_native", "shadow_coll", "cull_native", "cull_parallel", "shadow_cull", "music_hw_check", "music_hw_check_dump", "music_hw_ahead", "dev_mixer_stats", "dev_snd_watch", "audio_mixer_split", "audio_split_check",
    "sampler", "sampler_interval_us", "sampler_copies", "sampler_thread", "sampler_from_race",
    "draw_native", "shadow_draw", "flush_native", "flush_async", "flush_async_core", "flush_async_priority",
    "flush_async_frame", "flares_native", "flares_parallel", "shadow_flares",
    "load_native", "shadow_load", "math_native", "shadow_math", "particles_native", "particles_update",
    "particles_parallel", "particles_direct", "shadow_particles", "prep_native", "shadow_prep",
    "native_shadow", "native_shadow_rate",
    // Movies through the platform video decoder (src/ug2/movies.cpp).
    "movies", "movies_dir", "movies_selftest",
    // Front end (src/ug2/frontend.cpp).
    "boot_script", "fe_log", "fe_files", "ime_test", "ime_test_field", "movies_skip", "fe_msg", "fe_orbit", "fe_grep_words", "fe_grep", "fe_grep_frame", "fe_scan_file", "fe_scan_every", "boot_steps", "fe_dump", "boot_quit_after", "fe_gfx_apply", "fe_gfx_apply_delay",
    // Save folder (src/lib/file.cpp).
    "save_dir", "save_log", "save_sync", "save_atomic", "file_lazy_seek", "file_handle_cache",
    // Network play (src/lib/winapi/ws2_32.cpp, src/lib/winapi/revived).
    "network", "online_server", "revived_server", "revived_name", "revived_passphrase",
    // Options menu (src/ug2/options_menu.cpp).
    "options_menu",
    // Host build.
    "host_gpu_latency", "limiter_log", "debug_scene_finish", "display_vblank_wait", "wait_probe", "frame_stats", "d3d_fast_effects", "d3d_fast_draw", "d3d_fast_layer", "flush_direct", "host_check_ranges",
};

bool isKnown(const char* key)
{
    for (const char* known : Known)
    {
        if (strcmp(known, key) == 0)
            return true;
    }
    return false;
}

struct Entry
{
    char key[MaxKey];
    char value[MaxValue];
};

Entry s_entries[MaxEntries];
int   s_count = 0;

const char* find(const char* key)
{
    for (int i = 0; i < s_count; ++i)
    {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].value;
    }
    return nullptr;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Trims spaces and line ends in place; returns the trimmed start.
char* trim(char* text)
{
    while (isSpace(*text))
        text++;
    char* end = text + strlen(text);
    while (end > text && isSpace(end[-1]))
        *--end = 0;
    return text;
}

// Reads one line into line; false at the end of the file. A line longer than the buffer is consumed whole and
// returned as too long.
bool readLine(FILE* file, char* line, int size, bool* tooLong)
{
    if (!fgets(line, size, file))
        return false;
    *tooLong = false;
    if (!strchr(line, '\n') && !feof(file))
    {
        *tooLong = true;
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n')
        {
        }
    }
    return true;
}

}

// A missing file is created from missingFileText, when given.
void Config::load(const char* path, const char* missingFileText)
{
    s_count = 0;
    FILE* file = fopen(path, "rt");
    if (!file)
    {
        if (missingFileText)
            if (FILE* created = fopen(path, "wt"))
            {
                fputs(missingFileText, created);
                fclose(created);
            }
        return;
    }
    char buffer[MaxLine];
    bool tooLong = false;
    for (int number = 1; readLine(file, buffer, sizeof(buffer), &tooLong); ++number)
    {
        char* line = buffer;
        if (number == 1 && strncmp(line,"\xef\xbb\xbf", 3) == 0)
            line += 3;
        if (tooLong)
        {
            logError("config line %d: longer than %d characters, ignored", number, MaxLine - 2);
            continue;
        }
        line = trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char* separator = strchr(line, '=');
        if (!separator)
        {
            logError("config line %d: no '=', ignored", number);
            continue;
        }
        *separator = 0;
        const char* key = trim(line);
        const char* value = trim(separator + 1);
        if (!key[0] || strlen(key) >= MaxKey || strlen(value) >= MaxValue)
        {
            logError("config line %d: empty key, key over %d or value over %d characters, ignored", number,
                     MaxKey - 1, MaxValue - 1);
            continue;
        }
        const bool known = isKnown(key);
        if (find(key))
        {
            if (known)
                logError("config line %d: %s repeated, first value kept", number, key);
            continue;
        }
        if (s_count == MaxEntries)
        {
            logError("config line %d: more than %d entries, %s ignored", number, MaxEntries, key);
            continue;
        }
        // Unknown keys are kept too, so that a repeat is not reported again; nothing reads them.
        Entry& entry = s_entries[s_count++];
        strcpy(entry.key, key);
        strcpy(entry.value, value);
        if (known)
            logInfo("config %s=%s", entry.key, entry.value);
        else
            logError("config: unknown setting '%s'", entry.key);
    }
    fclose(file);
}

bool Config::tryGetBool(const char* key, bool* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    static const char* const On[] = { "on", "true", "yes", "1" };
    static const char* const Off[] = { "off", "false", "no", "0" };
    for (int i = 0; i < 4; ++i)
    {
        if (strcasecmp(text, On[i]) == 0 || strcasecmp(text, Off[i]) == 0)
        {
            *value = strcasecmp(text, On[i]) == 0;
            return true;
        }
    }
    logError("config %s=%s is not a boolean (on/off, true/false, yes/no, 1/0), ignored", key, text);
    return false;
}

bool Config::tryGetInt(const char* key, int* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    char* end = nullptr;
    errno = 0;
    const long number = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
    {
        logError("config %s=%s is not an integer, ignored", key, text);
        return false;
    }
    *value = int(number);
    return true;
}

bool Config::getBool(const char* key, bool defaultValue)
{
    bool value = defaultValue;
    return tryGetBool(key, &value) ? value : defaultValue;
}

int Config::getInt(const char* key, int defaultValue)
{
    int value = defaultValue;
    return tryGetInt(key, &value) ? value : defaultValue;
}

const char* Config::getString(const char* key, const char* defaultValue)
{
    const char* value = find(key);
    return value ? value : defaultValue;
}

int Config::getChoice(const char* key, const char* const* names, int count, int defaultIndex)
{
    const char* text = find(key);
    if (!text)
        return defaultIndex;
    for (int i = 0; i < count; ++i)
    {
        if (strcasecmp(text, names[i]) == 0)
            return i;
    }
    char list[MaxLine] = "";
    for (int i = 0; i < count; ++i)
    {
        if (i)
            strncat(list, "/", sizeof(list) - strlen(list) - 1);
        strncat(list, names[i], sizeof(list) - strlen(list) - 1);
    }
    logError("config %s=%s is not one of %s, %s used", key, text, list, names[defaultIndex]);
    return defaultIndex;
}

}
