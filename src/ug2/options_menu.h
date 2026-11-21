#ifndef UG2_OPTIONS_MENU_H_
#define UG2_OPTIONS_MENU_H_

#include <string>

// The Vita Settings model: the settings of the browser version (NFSU2 Revived, nfsu2.xyz) that the Vita has, grouped
// as its Settings page groups them. They are written to the same files as a hand edit: NeedForSpeedUnderground2.cfg
// and, for free roam, the PC mod's scripts/NFSU2FreeRoam.ini, and apply when the game starts again. The game's own
// screens show them (menu_entry.cpp).

namespace ug2 { namespace options {

// Before the game starts: the settings file and the game folder (the one with scripts/).
// options_menu=off in the settings file leaves the Vita Settings icon out.
void configure(const char* settingsPath, const char* gameFolder);
bool enabled();

// Values as the files hold them now; changes stay here until save().
void load();
bool dirty();
bool save();
// Starts the game again (does not return on the Vita).
void restart();

int groupCount();
const char* groupTitle(int g);
int itemCount(int g);
const char* itemLabel(int g, int i);
bool itemIsText(int g, int i);
bool itemIsSecret(int g, int i);
int itemMaxLength(int g, int i);
// The value as shown: an option's name, a text, or Set / Not set for a secret.
std::string itemValue(int g, int i);
// A text item's text to edit (empty for a secret).
std::string itemText(int g, int i);
// A row that does something (the cloud saves): Cross runs it, its value says how it went, and actionVersion changes
// with each new status.
bool itemIsAction(int g, int i);
void runAction(int g, int i);
unsigned actionVersion();
// The next or previous option of a choice.
void cycle(int g, int i, int delta);
// A text item's new text; an empty one leaves a secret as it was.
void setText(int g, int i, const std::string& text);
// After a change was saved: applies it now if it can be, which most settings cannot (true then).
bool applyLive(int g, int i);

}}

#endif /* !UG2_OPTIONS_MENU_H_ */
