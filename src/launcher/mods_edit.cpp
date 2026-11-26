// The launcher's side of the mod registry: the Mods tab's rows and edits of the mods' ini files through mINI.

#include "mods_edit.h"
#include "mods_hook.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/cfgfile.h>
#include <platform/log.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace ug2 { namespace mods {

namespace
{

bool writeRaw(const std::string& file, const std::string& text)
{
    FILE* out = fopen(file.c_str(), "wb");
    if (!out)
        return false;
    const bool written = fwrite(text.data(), 1, text.size(), out) == text.size();
    return fclose(out) == 0 && written;
}

// mINI writes LF line ends; the file gets back the CRLF it had.
std::string withCrLf(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text)
    {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

}

namespace ini
{

bool set(const std::string& file, const char* section, const char* key, const std::string& number,
         const char* comment)
{
    const std::string temporary = file + ".tmp";
    std::string text;
    const bool existed = pf::cfgfile::readFile(file.c_str(), &text);
    if (existed && !writeRaw(temporary, text))
        return false;
    const size_t firstBreak = text.find('\n');
    const bool crlf = firstBreak == std::string::npos || (firstBreak > 0 && text[firstBreak - 1] == '\r');
    const bool endsWithBreak = !existed || (!text.empty() && text.back() == '\n');
    mINI::INIFile ini(temporary);
    mINI::INIStructure data;
    if (fileExists(temporary) && !ini.read(data))
        return false;
    std::string value = number;
    if (!(data.has(section) && data[section].has(key)) && comment && *comment)
    {
        const size_t used = strlen(key) + 3 + number.size();
        value += std::string(used < 36 ? 36 - used : 1, ' ') + "// " + comment;
    }
    data[section][key] = value;
    std::string written;
    if (!ini.write(data, true) || !pf::cfgfile::readFile(temporary.c_str(), &written))
        return false;
    if (!written.empty() && written.back() == '\n' && !endsWithBreak)
        written.pop_back();
    else if (endsWithBreak && (written.empty() || written.back() != '\n'))
        written += '\n';
    if (!writeRaw(temporary, crlf ? withCrLf(written) : written))
        return false;
    if (rename(temporary.c_str(), file.c_str()) == 0)
        return true;
    // The Vita refuses to rename over an existing file.
    return remove(file.c_str()) == 0 && rename(temporary.c_str(), file.c_str()) == 0;
}

}

std::vector<cfgui::ModView> views(const std::string& gameFolder)
{
    std::vector<cfgui::ModView> result;
    for (int s = 0; s < SourceCount; ++s)
    {
        const Source& src = source(SourceId(s));
        const std::string file = pathIn(gameFolder, src.iniPath);
        std::string text;
        const pf::cfgfile::FileState fileState = pf::cfgfile::readText(file.c_str(), &text);
        const bool found = fileState == pf::cfgfile::FileState::Readable;
        for (const IniKey& key : src.keys)
        {
            cfgui::ModView view;
            view.group = heading(src);
            view.file = file;
            view.fileState = fileState;
            if (fileState == pf::cfgfile::FileState::Unreadable)
                view.warning = modUnreadable(src.mod, file);
            int feature = -1;
            for (int i = 0; i < Count; ++i)
            {
                const Mod& mod = get(Id(i));
                if (&source(mod.source) == &src && !strcmp(def(mod).section, key.section) && !strcmp(def(mod).key, key.key))
                    feature = i;
            }
            if (!key.supported || feature < 0)
            {
                std::string value;
                view.name = std::string(key.section) + ": " + key.key;
                view.value = found && ini::get(file, key.section, key.key, &value) ? value : key.def;
                view.supported = false;
                result.push_back(view);
                continue;
            }
            const Mod& mod = get(Id(feature));
            const State s = state(mod, gameFolder);
            view.id = feature;
            view.name = def(mod).name;
            view.enabled = s.on;
            if (!s.met)
                view.requirements = s.requirements;
            if (src.downloadable())
            {
                view.downloadName = src.mod;
                view.downloadSpace = src.download.spaceNeeded();
            }
            view.details.push_back(mod.description);
            if (mod.note)
                view.details.push_back(mod.note);
            result.push_back(view);
        }
    }
    return result;
}

bool save(const Mod& mod, const std::string& gameFolder, bool on)
{
    const std::string file = pathIn(gameFolder, source(mod.source).iniPath);
    std::string value;
    const bool found = fileExists(file);
    if (!found && !on)
        return true;
    if (found && (ini::get(file, def(mod).section, def(mod).key, &value) ? isOn(value) : onByDefault(mod)) == on)
        return true;
    if (!found)
        mkdir(file.substr(0, file.rfind('/')).c_str(), 0777);
    if (!ini::set(file, def(mod).section, def(mod).key, on ? "1" : "0", mod.comment))
    {
        pf::logError("mods: writing %s failed", file.c_str());
        return false;
    }
    pf::logInfo("mods: %s %s %s (%s = %d in %s)", source(mod.source).mod, def(mod).name, on ? "on" : "off", def(mod).key,
                on ? 1 : 0, file.c_str());
    return true;
}


}}
