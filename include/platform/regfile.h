#ifndef PLATFORM_REGFILE_H_
#define PLATFORM_REGFILE_H_

#include <string>

// Text edits of the registry file (registry.txt, written by src/lib/registry.cpp): string values, other lines stay byte for byte.

namespace pf
{
namespace regfile
{

// A key's REG_SZ value, without its end byte.
bool findString(const std::string& text, const std::string& key, const std::string& name, std::string* value);
// text with the string value set; the key and the value are added when missing.
std::string setString(const std::string& text, const std::string& key, const std::string& name, const std::string& value);

}
}

#endif
