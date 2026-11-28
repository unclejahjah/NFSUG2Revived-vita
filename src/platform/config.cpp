#include <platform/config.h>
#include <platform/cfgfile.h>
#include <platform/log.h>
#include <platform/settings.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <strings.h>
#include <vector>

namespace pf
{

namespace
{

const int MaxKey     = 32;
const int MaxValue   = 384;
const int MaxLine    = 448;

bool isKnown(const char* key)
{
    return findSetting(canonicalKey(key)) != nullptr;
}

struct Entry
{
    char key[MaxKey];
    char value[MaxValue];
    bool fromAlias;     // set under an old name; a line with the current name replaces it
};

std::vector<Entry> s_entries;
std::vector<std::pair<std::string, std::string>> s_retiredValues;
std::vector<std::string> s_pending;     // keys set since the last save, in order
std::string s_path;

// A secret (revived_passphrase) is never logged.
bool isSecret(const char* key)
{
    return strstr(key, "passphrase") || strstr(key, "password");
}

const char* find(const char* wanted)
{
    const char* key = canonicalKey(wanted);
    for (const Entry& entry : s_entries)
    {
        if (strcmp(entry.key, key) == 0)
            return entry.value[0] ? entry.value : nullptr;    // set() empties a removed key
    }
    return nullptr;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Trims spaces and line ends in place; returns the trimmed start.
char* trim(char* text)
{
    while (isSpace(*text))
        text++;
    char* end = text + strlen(text);
    while (end > text && isSpace(end[-1]))
        *--end = 0;
    return text;
}

// Reads one line into line; false at the end of the file. A line longer than the buffer is consumed whole and
// returned as too long.
bool readLine(FILE* file, char* line, int size, bool* tooLong)
{
    if (!fgets(line, size, file))
        return false;
    *tooLong = false;
    if (!strchr(line, '\n') && !feof(file))
    {
        *tooLong = true;
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n')
        {
        }
    }
    return true;
}

}

const char* Config::getRetired(const char* key)
{
    for (const auto& old : s_retiredValues)
    {
        if (old.first == key)
            return old.second.c_str();
    }
    return nullptr;
}

// A missing file is created from missingFileText, when given.
bool Config::load(const char* path, const char* missingFileText)
{
    s_entries.clear();
    s_retiredValues.clear();
    s_pending.clear();
    s_path = path;
    std::string text;
    if (cfgfile::readText(path, &text) == cfgfile::FileState::Unreadable)
    {
        logError("config: %s is unreadable, all settings at their defaults", path);
        return false;
    }
    FILE* file = fopen(path, "rt");
    if (!file)
    {
        if (missingFileText)
            if (FILE* created = fopen(path, "wt"))
            {
                fputs(missingFileText, created);
                fclose(created);
            }
        return true;
    }
    char buffer[MaxLine];
    bool tooLong = false;
    for (int number = 1; readLine(file, buffer, sizeof(buffer), &tooLong); ++number)
    {
        char* line = buffer;
        if (number == 1 && strncmp(line,"\xef\xbb\xbf", 3) == 0)
            line += 3;
        if (tooLong)
        {
            logError("config line %d: longer than %d characters, ignored", number, MaxLine - 2);
            continue;
        }
        line = trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char* separator = strchr(line, '=');
        if (!separator)
        {
            logError("config line %d: no '=', ignored", number);
            continue;
        }
        *separator = 0;
        const char* key = trim(line);
        const char* value = trim(separator + 1);
        if (!key[0] || strlen(key) >= MaxKey || strlen(value) >= MaxValue)
        {
            logError("config line %d: empty key, key over %d or value over %d characters, ignored", number,
                     MaxKey - 1, MaxValue - 1);
            continue;
        }
        if (settingRetired(key))
        {
            bool seen = false;
            for (const auto& old : s_retiredValues)
                seen = seen || old.first == key;
            if (!seen)
                s_retiredValues.emplace_back(key, value);
            continue;
        }
#ifndef DEV_SETTINGS
        if (const Setting* setting = findSetting(canonicalKey(key)); setting && setting->group == SettingGroup::Developer)
        {
            logInfo("config %s ignored: a developer setting, read only by a DEV_SETTINGS build", key);
            continue;
        }
#endif
        const bool known = isKnown(key);
        const char* current = canonicalKey(key);
        const bool alias = current != key;
        Entry* existing = nullptr;
        for (Entry& entry : s_entries)
        {
            if (strcmp(entry.key, current) == 0)
                existing = &entry;
        }
        if (existing)
        {
            if (existing->fromAlias && !alias)
            {
                strcpy(existing->value, value);
                existing->fromAlias = false;
                logInfo("config %s=%s", existing->key, isSecret(existing->key) ? "(set, not logged)" : existing->value);
            }
            else if (known)
                logError("config line %d: %s repeated, first value kept", number, key);
            continue;
        }
        // Unknown keys are kept too, so that a repeat is not reported again; nothing reads them.
        s_entries.emplace_back();
        Entry& entry = s_entries.back();
        strcpy(entry.key, current);
        strcpy(entry.value, value);
        entry.fromAlias = alias;
        const char* shown = isSecret(entry.key) ? (*entry.value ? "(set, not logged)" : "") : entry.value;
        if (alias)
            logInfo("config %s is now %s, the old name still works: %s=%s", key, current, current, shown);
        else if (known)
            logInfo("config %s=%s", entry.key, shown);
        else
            logError("config: unknown setting '%s'", entry.key);
    }
    fclose(file);
    return true;
}

static unsigned s_generation = 0;

// In memory, as if the file had it (an empty value removes the key); save() writes keys set since into the file.
// Readers that cache a value compare generation().
void Config::set(const char* key, const char* value)
{
    const char* current = canonicalKey(key);
    if (strlen(current) >= MaxKey || strlen(value) >= MaxValue)
    {
        logError("config: %s not set, key over %d or value over %d characters", key, MaxKey - 1, MaxValue - 1);
        return;
    }
    Entry* entry = nullptr;
    for (Entry& e : s_entries)
    {
        if (strcmp(e.key, current) == 0)
            entry = &e;
    }
    if (!entry && *value)
    {
        s_entries.emplace_back();
        entry = &s_entries.back();
        strcpy(entry->key, current);
    }
    if (entry)
    {
        strcpy(entry->value, value);     // empty: find() treats the key as removed
        entry->fromAlias = false;
    }
    ++s_generation;
    for (const std::string& pending : s_pending)
    {
        if (pending == current)
            return;
    }
    s_pending.push_back(current);
}

bool Config::save()
{
    if (s_pending.empty())
        return true;
    if (s_path.empty())
        return false;
    std::string text;
    if (cfgfile::readText(s_path.c_str(), &text) == cfgfile::FileState::Unreadable)
    {
        logError("config: %s is unreadable, nothing saved", s_path.c_str());
        return false;
    }
    std::vector<cfgfile::Change> changes;
    for (const std::string& key : s_pending)
    {
        cfgfile::Change change;
        change.key = key;
        change.value = find(key.c_str());
        const Setting* setting = findSetting(key.c_str());
        change.atDefault = setting && !(setting->flags & SettingAuto) && change.value == defaultText(*setting);
        changes.push_back(change);
    }
    if (!cfgfile::writeFile(s_path.c_str(), cfgfile::apply(text, changes)))
    {
        logError("config: cannot write %s", s_path.c_str());
        return false;
    }
    s_pending.clear();
    return true;
}

bool Config::tryGetBool(const char* key, bool* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    if (cfgfile::parseBool(text, value))
        return true;
    logError("config %s=%s is not a boolean (on/off, true/false, yes/no, 1/0), ignored", key, text);
    return false;
}

bool Config::tryGetInt(const char* key, int* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    if (cfgfile::parseInt(text, value))
        return true;
    logError("config %s=%s is not an integer, ignored", key, text);
    return false;
}

bool Config::getBool(const char* key, bool defaultValue)
{
    bool value = defaultValue;
    return tryGetBool(key, &value) ? value : defaultValue;
}

int Config::getInt(const char* key, int defaultValue)
{
    int value = defaultValue;
    return tryGetInt(key, &value) ? value : defaultValue;
}

unsigned Config::generation()
{
    return s_generation;
}

const char* Config::getString(const char* key, const char* defaultValue)
{
    const char* value = find(key);
    return value ? value : defaultValue;
}

int Config::getChoice(const char* key, const char* const* names, int count, int defaultIndex)
{
    const char* text = find(key);
    if (!text)
        return defaultIndex;
    for (int i = 0; i < count; ++i)
    {
        if (strcasecmp(text, names[i]) == 0)
            return i;
    }
    char list[MaxLine] = "";
    for (int i = 0; i < count; ++i)
    {
        if (i)
            strncat(list, "/", sizeof(list) - strlen(list) - 1);
        strncat(list, names[i], sizeof(list) - strlen(list) - 1);
    }
    logError("config %s=%s is not one of %s, %s used", key, text, list, names[defaultIndex]);
    return defaultIndex;
}

}
