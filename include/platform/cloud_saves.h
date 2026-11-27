#ifndef PLATFORM_CLOUD_SAVES_H_
#define PLATFORM_CLOUD_SAVES_H_

#include <string>

// The browser version's cloud saves (nfsu2.xyz /vault, web/src/app/vault.ts) for the Vita's save folder. They are
// kept under the passphrase of revived_passphrase: the saves go up as a zip, sealed on the Vita with AES-256-GCM, and
// the server never sees the passphrase or a save. A backup of the saves here is made before cloud saves replace them.
// The work runs on a thread of its own; status() says how it went, in a few words.

namespace pf { namespace cloud
{

enum class Action
{
    Sync,       // the newer side wins; both changed: nothing happens, the player picks
    Upload,     // the saves here replace the cloud's
    Download,   // the cloud's replace the saves here
};

// The game folder (the one with save/).
void configure(const std::string& gameFolder);
// The save folder (save_dir, else the game folder's save/), without a trailing '/'.
std::string saveRoot();
// Starts an action on its thread; false while one runs, or when it cannot start (the status says why).
bool start(Action action);
bool busy();
// Changes with every new status.
unsigned version();
std::string status();

}}

#endif /* !PLATFORM_CLOUD_SAVES_H_ */
