#ifndef UG2_LANGUAGE_H_
#define UG2_LANGUAGE_H_

#include <string>

// The game's languages: the registry's Language value names one, and its files must be in the game folder.

namespace ug2 { namespace language {

const int Count = 11;
extern const char* const Names[Count];          // the game's table at 0x7F7020, by language index
extern const char* const RegistryNames[Count];  // the registry's Language value that gives each one
extern const char DefaultRegistryName[];        // the value the port seeds: the game's own default

// Language index a name or registry value gives, or -1.
int indexOf(const char* name);
int indexOfRegistryName(const char* value);
// Whether the language's files are in the game folder.
bool available(const std::string& gameFolder, int index);
// The settings value, such as french, for a registry value; default for English US and unknown ones.
std::string optionOf(const std::string& registryValue);
// The registry value for a settings value; empty for default.
std::string registryValueOf(const std::string& option);
// After the registry loads: a language without its files gives way to the default.
void configure(const std::string& gameFolder);

}}

#endif /* !UG2_LANGUAGE_H_ */
