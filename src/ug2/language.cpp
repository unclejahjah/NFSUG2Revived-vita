// The game's languages: the registry's Language value names one, and its files must be in the game folder.

#include "language.h"
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <strings.h>

namespace ug2 { namespace language {

const char* const Names[Count] = { "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "DUTCH", "SWEDISH", "DANISH",
                                   "KOREAN", "CHINESE", "JAPANESE" };
const char* const RegistryNames[Count] = { "English US", "French", "German", "Italian", "Spanish", "Dutch", "Swedish",
                                           "Danish", "Korean", "Chinese (Traditional)", "Japanese" };
const char DefaultRegistryName[] = "English US";

namespace
{

bool findEntry(const std::string& folder, const std::string& name, std::string* found)
{
    DIR* dir = opendir(folder.c_str());
    if (!dir)
        return false;
    bool matched = false;
    while (const dirent* entry = readdir(dir))
    {
        if (strcasecmp(entry->d_name, name.c_str()) == 0)
        {
            *found = entry->d_name;
            matched = true;
            break;
        }
    }
    closedir(dir);
    return matched;
}

bool hasFile(const std::string& gameFolder, const char* folder, const std::string& name)
{
    std::string directory, file;
    return findEntry(gameFolder, folder, &directory) && findEntry(gameFolder + "/" + directory, name, &file);
}

}

int indexOf(const char* name)
{
    for (int i = 0; i < Count; ++i)
    {
        if (strcasecmp(name, Names[i]) == 0)
            return i;
    }
    return -1;
}

int indexOfRegistryName(const char* value)
{
    for (int i = 0; i < Count; ++i)
    {
        if (strcasecmp(value, RegistryNames[i]) == 0)
            return i;
    }
    return strcasecmp(value, "English UK") == 0 ? 0 : -1;
}

bool available(const std::string& gameFolder, int index)
{
    if (index < 0 || index >= Count)
        return false;
    const std::string name = Names[index];
    return hasFile(gameFolder, "LANGUAGES", name + ".BIN") && hasFile(gameFolder, "memcard", "LOCALE_" + name + ".loc");
}

std::string optionOf(const std::string& registryValue)
{
    const int index = indexOfRegistryName(registryValue.c_str());
    if (index <= 0)
        return "default";
    std::string option = Names[index];
    for (char& c : option)
        c = char(tolower(static_cast<unsigned char>(c)));
    return option;
}

std::string registryValueOf(const std::string& option)
{
    const int index = indexOf(option.c_str());
    return index <= 0 ? std::string() : std::string(RegistryNames[index]);
}

}}
