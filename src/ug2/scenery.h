#ifndef UG2_SCENERY_H_
#define UG2_SCENERY_H_

#include <cstdint>

// Scenery small-object cull (dev_world_min_pixels, dev_reflection_min_pixels) and distant LOD
// (dev_world_lod_pixels). The scene log (scene_log) is a development tool in dev/scene_log.h, compiled in only with
// DEV_TOOLS.

namespace ug2 { namespace scenery {

enum Phase { PhaseCull, PhaseDraw, PhaseFlush, PhaseCount };     // 0x5d69b0, 0x5d6ad0, 0x5cf420

struct Settings
{
    float mainMinPixels;
    float otherMinPixels;
    float lodPixels;    // dev_world_lod_pixels: narrower scenery uses its reflection model in the player views
    uint32_t lodMinTris;        // dev_world_lod_min_tris: normal models with fewer triangles keep their model
    uint32_t lodMaxPercent;     // dev_world_lod_max_percent: the reflection model's triangles, at most this share
    float lodMinWidth;          // dev_world_lod_min_width: metres; narrower types keep their model
    bool lodSigns;              // dev_world_lod_signs: off keeps every sign type (XS_ names) on its normal model
    bool keepLights;    // dev_world_min_pixels_lights: models with light markers are never culled
};

extern Settings g_settings;

// Before guest code runs: reads the keys.
void configure();

}}

#ifdef DEV_TOOLS
#include "../dev/scene_log.h"
#else
#define SCENE_STATS(...)

namespace ug2 { namespace scenery {

inline void configureLog() {}
inline constexpr bool logging() { return false; }

template <typename Body>
inline void timed(Phase, Body body)
{
    body();
}

}}
#endif

#endif /* !UG2_SCENERY_H_ */
