#ifndef LAUNCHER_UNPACK_H_
#define LAUNCHER_UNPACK_H_

// Mod archives (zip, 7z, rar, rar5) read with libarchive; only the listed entries are written, by plain stdio.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace launcher { namespace unpack {

struct Entry
{
    std::string from;           // name in the archive, '/' separated, matched without case
    std::string path;           // where to write it
    uint64_t bytes;
};

// Relative, without "..", ".", empty parts or a drive.
bool safeName(const std::string& name);

// Writes each entry; unsafe names, missing entries, wrong sizes fail.
bool extract(const std::string& archive, const std::vector<Entry>& entries, const std::atomic<bool>& abort,
             std::atomic<uint64_t>* done, std::string* error);

}}}

#endif
