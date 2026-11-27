#ifndef UG2_TRIPCOVER_H_
#define UG2_TRIPCOVER_H_

// The cover over the game's menus while a free roam race's trip drives them (roam_game.c mp_cover), as the browser
// version's: a dark backdrop, the race, its players or results, and where the trip is with a spinner.

namespace ug2 { namespace tripcover {

// Game thread, each frame of the trip: the cover's lines (roam_game.c mp_cover's format), or null for no cover.
void set(const char* lines);

}}

#endif /* !UG2_TRIPCOVER_H_ */
