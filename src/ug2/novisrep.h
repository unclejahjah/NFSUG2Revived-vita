#ifndef UG2_NOVISREP_H_
#define UG2_NOVISREP_H_

// NFSU/NFSU2 Visual Reputation Remover 1.1 (NFSU2 half) as the port knows it: its settings file, patch addresses and ratings.

#include <cstdint>

namespace ug2 { namespace novisrep {

const char* const Name = "NFSU/NFSU2 Visual Reputation Remover";
const char* const Version = "1.1";
const char* const Author = "thelink2012";
const char* const IniPath = "scripts/NFSU2VitaNoVisualRep.ini";   // the mod has no ini of its own; relative to the game folder
const char* const Section = "NFSU2";
const char* const Key = "RemoveVisualRep";

const uint32_t CallSite = 0x557eeb;         // guard byte 0xE8: the call after the NOP run
const uint32_t NopStart = 0x557ede;         // MakeNOP(0x557EDE, 0x12) inside the customization screen's constructor 0x557EC0
const uint32_t NopLength = 0x12;
const uint32_t RatingFunction = 0x5234c0;   // guard byte 0x55: replaced by a JMP to CalculateVisualRep
const uint32_t StageAddress = 0x862838;     // the current game stage

// Stage 0..6 are 0, 1, 3, 5.5, 8, 10, 10; anything else 10, as the mod's switch.
inline float rating(int32_t stage)
{
    switch (stage)
    {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return 3.0f;
    case 3: return 5.5f;
    case 4: return 8.0f;
    default: return 10.0f;
    }
}

}}

#endif
