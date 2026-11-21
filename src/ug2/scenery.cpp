// Scenery settings (scenery.h): the small-object cull and the distant LOD.

#include "scenery.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <algorithm>

namespace ug2 { namespace scenery {

Settings g_settings = {};

void configure()
{
    const int world = std::min(std::max(pf::Config::getInt("dev_world_min_pixels", 0), 0), 64);
    const int other = std::min(std::max(pf::Config::getInt("dev_reflection_min_pixels", world), 0), 64);
    g_settings.mainMinPixels = float(world);
    g_settings.otherMinPixels = float(other);
    const int lod = std::min(std::max(pf::Config::getInt("dev_world_lod_pixels", 0), 0), 100000);
    g_settings.lodPixels = float(lod);
    g_settings.lodMinTris = uint32_t(std::min(std::max(pf::Config::getInt("dev_world_lod_min_tris", 24), 0), 100000));
    g_settings.lodMaxPercent =
        uint32_t(std::min(std::max(pf::Config::getInt("dev_world_lod_max_percent", 75), 0), 100));
    g_settings.lodMinWidth = float(std::min(std::max(pf::Config::getInt("dev_world_lod_min_width", 0), 0), 10000));
    g_settings.lodSigns = pf::Config::getBool("dev_world_lod_signs", true);
    g_settings.keepLights = pf::Config::getBool("dev_world_min_pixels_lights", true);
    pf::logInfo("scenery: minimum width %d px in the player views (dev_world_min_pixels), %d px in the others "
                "(dev_reflection_min_pixels), models with lights %s; reflection models below %d px (dev_world_lod_pixels) "
                "for types of %u triangles or more whose reflection model has at most %u%% of them, %d m wide or more, "
                "signs %s", world,
                other, g_settings.keepLights ? "kept" : "culled too", lod, unsigned(g_settings.lodMinTris),
                unsigned(g_settings.lodMaxPercent), int(g_settings.lodMinWidth),
                g_settings.lodSigns ? "included (dev_world_lod_signs)" : "excluded");
    configureLog();
}

}}
