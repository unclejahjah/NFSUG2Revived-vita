// The front-end button icons as one texture pack in the layout of the game's own TPK files, built in memory.

#include "helpicons.h"
#include "helpicons_art.h"
#include "controlspack.h"
#include "psmap.h"

namespace ug2 { namespace helpicons {

std::vector<uint8_t> buildPack()
{
    return controlspack::buildPack(psmap::AtlasName, helpiconsart::Atlas, sizeof(helpiconsart::Atlas));
}

std::vector<uint8_t> buildTintPack()
{
    std::vector<uint8_t> atlas(helpiconsart::Atlas, helpiconsart::Atlas + sizeof(helpiconsart::Atlas));
    for (size_t block = 0; block + 16 <= atlas.size(); block += 16)
    {
        for (size_t i = 8; i < 16; ++i)
            atlas[block + i] = i < 12 ? 0xFF : 0;
    }
    return controlspack::buildPack(psmap::TintAtlasName, atlas.data(), atlas.size());
}

}}
