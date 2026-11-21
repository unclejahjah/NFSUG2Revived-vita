#ifndef UG2_EXOPTS_H_
#define UG2_EXOPTS_H_

// NFSU2 Extra Options as the port knows it: where its settings file is and the keys the port reads.

namespace ug2 { namespace exopts {

const char* const Name = "ExtraOptions";
const char* const Version = "5.1.0.1340";
const char* const Author = "ExOpts Team";
const char* const IniPath = "scripts/NFSU2ExtraOptionsSettings.ini";   // relative to the game folder

const char* const RegionSection = "Gameplay";
const char* const RegionKey = "UnlockRegionalCars";

}}

#endif
