#ifndef LAUNCHER_NET_H_
#define LAUNCHER_NET_H_

// HTTP(S) downloads with libcurl for the Mods tab; TLS peer and host are verified against a CA file.

#include <atomic>
#include <cstdint>
#include <string>

namespace ug2 { namespace launcher { namespace net {

// On the Vita: the network modules, and whether Wi-Fi is connected.
bool start(const char* caFile, std::string* error);
void stop();

enum class Result { Ok, Failed, Cancelled };

// Appends from *have to path, at most limit bytes; restarts once when the server ignores the range.
Result download(const std::string& url, const std::string& path, uint64_t* have, uint64_t limit,
                const std::atomic<bool>* abort, std::atomic<uint64_t>* done, std::string* error);

}}}

#endif
