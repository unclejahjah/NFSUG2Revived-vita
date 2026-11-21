#ifndef PLATFORM_CFGFILE_H_
#define PLATFORM_CFGFILE_H_

#include <platform/textfile.h>
#include <string>
#include <vector>

namespace pf
{
namespace cfgfile
{

bool parseBool(const char* text, bool* value);  // on/off, true/false, yes/no, 1/0 in any case
bool parseInt(const char* text, int* value);    // decimal, whole text

// Value of key's first active line, old name included.
bool find(const std::string& text, const char* key, std::string* value);

// Setting key to value. An active line keeps everything but its value; a default goes back to the commented
// form only on a plain key=value line.
struct Change
{
    std::string key;
    std::string value;
    bool        atDefault = false;
};

// text with changes applied; untouched lines stay byte for byte.
std::string apply(const std::string& text, const std::vector<Change>& changes);

bool readFile(const char* path, std::string* text);

// Missing, readable or unreadable (pf::readTextFile).
using FileState = TextFile;
inline FileState readText(const char* path, std::string* text) { return readTextFile(path, text); }
bool writeFile(const char* path, const std::string& text);     // through path.tmp and a rename

}
}

#endif
