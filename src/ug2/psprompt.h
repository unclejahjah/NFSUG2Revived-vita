#ifndef UG2_PSPROMPT_H_
#define UG2_PSPROMPT_H_

#include <lib/winapp.h>
#include <recomp.h>

// PS2-style front-end prompts: button icons for the text tokens and Triangle as Help.

namespace ug2 { namespace psprompt {

// Before guest code runs: reads ps2_prompts and hooks the joy event callback.
void configure(win32::WinApplication& app);

// Which game function calls the token skip (sub_50bac0): the generated code leaves no return address to tell.
enum Context { Outside, Render, Width, Other };

struct Scope
{
    Context    context;
    x86::reg32 font;
};

Scope enterText(Context context, x86::reg32 font);
// The font of the latest text render or width call, 0 before any.
x86::reg32 lastFont();
void leaveText(const Scope& saved);

// Draws or measures a "$JOY_EVENT_...$" token's icon.
void beforeTokenSkip(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif /* !UG2_PSPROMPT_H_ */
