#ifndef UG2_OWNSCREENS_H_
#define UG2_OWNSCREENS_H_

#include "fepackage.h"

#include <lib/winapp.h>

#include <functional>
#include <string>
#include <vector>

// Port-owned FEng screens: renamed copies of FRONTB packages with baked edits, and a screen table the game reads first.

namespace ug2 { namespace ownscreens {

typedef std::function<void(fepackage::Package&)> Edit;

typedef std::function<bool(fepackage::Package&)> Replace;

// Reads a package from an archive file of the game folder (guest path such as C:\FRONTEND\FrontB.lzc); false when
// the file or the package is missing.
bool loadFromArchive(const std::string& guestPath, const std::string& name, fepackage::Package* out);
// Queues an edit of a package under its own name. It is applied to the chunk the loader registers (hook of 0x570D70),
// before the game creates the package's record; the edit returns false to keep the original.
void addReplacement(const std::string& name, const Replace& edit);
// The chunk the loader should register instead of this one, else 0 (a chunk to register as it is).
x86::reg32 replacementFor(x86::reg32 chunk);
// Our chunk registered for this one, else 0; forgets it.
x86::reg32 unloadFor(x86::reg32 chunk);
struct ReplacementState
{
    bool done = false;          // the loader was handed our chunk
    bool recordOurs = false;    // the package record's chunk is ours
    x86::reg32 instance = 0;    // the record's FEng package instance, 0 until the game instantiates it
};
ReplacementState replacementState(const std::string& name);
// Queues a renamed FRONTB package copy, registered after FRONTB loads.
void addCopy(const std::string& source, const std::string& name, const Edit& edit);
// Port table entry: UI_Options.fng's fields with this constructor.
void addScreen(const std::string& name, x86::reg32 constructor);
// The name as a permanent guest string.
x86::reg32 guestName(const std::string& name);
bool registered(const std::string& name);
// A FRONTB package as the game registered it, read from its chunk; false when not loaded or unreadable.
bool loadPackage(const std::string& name, fepackage::Package* out);
// Per frame on the game thread: registers the pending copies.
void frame(win32::WinApplication* app, x86::CPU& cpu);
// The game's hash of a package name.
x86::reg32 hashOfName(const std::string& name);
// Our entry for a package hash, else 0.
x86::reg32 entryOf(x86::reg32 hash);

}}

#endif /* !UG2_OWNSCREENS_H_ */
