#ifndef UG2_FREEROAM_H_
#define UG2_FREEROAM_H_

#include <x86.h>

namespace win32 { class WinApplication; }

// Free roam with other players: one step a frame, on the game's main thread, from GameLoop (src/ug2/freeroam.cpp).
namespace ug2 { namespace freeroam
{

void frame(win32::WinApplication* app, x86::CPU& cpu);

// After the HUD's update (src/ug2/freeroam.cpp): the names over friends' cars.
void hud(win32::WinApplication* app, x86::CPU& cpu);

// Free roam's guest calls (roam_win_call) on this CPU while the scope lasts: a patched game handler of free roam's
// multiplayer races (src/ug2/mprace_hooks.cpp). Inside free roam's own frame, the frame's CPU stays.
class GuestScope
{
public:
    GuestScope(win32::WinApplication* app, x86::CPU& cpu);
    ~GuestScope();
private:
    win32::WinApplication* m_app;
    x86::CPU* m_cpu;
};

}}

#endif
