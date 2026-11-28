// The launcher's side of the mod registry: the Mods tab's rows and edits of the mods' ini files through mINI.

#include "mods_edit.h"
#include "mods_hook.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/cfgfile.h>
#include <platform/log.h>
#include <cstdio>
#include <cstdlib>
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

bool set(const std::string& file, const std::vector<Line>& lines)
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
    for (const Line& line : lines)
    {
        if (line.value.empty())
        {
            if (data.has(line.section))
                data[line.section].remove(line.key);
            continue;
        }
        std::string value = line.value;
        if (!(data.has(line.section) && data[line.section].has(line.key)) && line.comment && *line.comment)
        {
            const size_t used = strlen(line.key) + 3 + line.value.size();
            value += std::string(used < 36 ? 36 - used : 1, ' ') + "// " + line.comment;
        }
        data[line.section][line.key] = value;
    }
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

bool set(const std::string& file, const char* section, const char* key, const std::string& number,
         const char* comment)
{
    return set(file, { Line{ section, key, number, comment } });
}

}

namespace
{

// As the Mods tab writes a number (cfgui::formatModValue).
std::string formatValue(double value)
{
    char text[32];
    snprintf(text, sizeof text, "%g", value);
    return text;
}

const Mod* modOf(const Source& src, const IniKey& key)
{
    for (int i = 0; i < Count; ++i)
    {
        const Mod& mod = get(Id(i));
        if (&source(mod.source) == &src && !strcmp(def(mod).section, key.section) && !strcmp(def(mod).key, key.key))
            return &mod;
    }
    return nullptr;
}

cfgui::ModKind kindOf(modreq::Kind kind)
{
    switch (kind)
    {
    case modreq::Kind::Int: return cfgui::ModKind::Int;
    case modreq::Kind::Float: return cfgui::ModKind::Float;
    case modreq::Kind::Enum: return cfgui::ModKind::Enum;
    default: return cfgui::ModKind::Bool;
    }
}

}

std::string vanillaValue(const Mod& mod)
{
    return def(mod).vanilla;
}

bool writeFresh(const Source& src, const std::string& file, const char* section, const char* key,
                const std::string& value)
{
    std::vector<ini::Line> lines;
    for (const IniKey& k : src.keys)
    {
        const bool chosen = section && !strcmp(k.section, section) && !strcmp(k.key, key);
        const Mod* mod = modOf(src, k);
        const std::string v = chosen ? value : mod ? vanillaValue(*mod) : std::string(k.def);
        if (!v.empty())
            lines.push_back(ini::Line{ k.section, k.key, v, mod ? mod->comment : nullptr });
    }
    mkdir(file.substr(0, file.rfind('/')).c_str(), 0777);
    return ini::set(file, lines);
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
                if (&source(mod.source) == &src && !strcmp(def(mod).section, key.section) &&
                    !strcmp(def(mod).key, key.key))
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
            const modreq::Setting& setting = def(mod);
            const State s = state(mod, gameFolder);
            view.id = feature;
            view.name = setting.name;
            view.kind = kindOf(setting.kind);
            view.min = setting.min;
            view.max = setting.max;
            view.step = setting.step;
            view.labels.assign(setting.labels.begin(), setting.labels.end());
            // ExtraOptions 5.1.0.1340 every key: deviation (user decision): default, reset and new file are vanilla
            view.reset = vanillaValue(mod);
            view.vanilla = vanillaValue(mod);
            view.vanillaChoice = view.vanilla.empty();
            if (s.keyFound)
                view.value = formatValue(view.kind == cfgui::ModKind::Bool ? s.value != 0 : s.value);
            else
                view.value = view.vanilla;
            view.enabled = s.keyFound ? s.on : view.vanilla != "0" && !view.vanilla.empty();
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

bool save(const Mod& mod, const std::string& gameFolder, const std::string& value)
{
    const Source& src = source(mod.source);
    const std::string file = pathIn(gameFolder, src.iniPath);
    const State s = state(mod, gameFolder);
    const bool found = s.iniFound && !s.unreadable;
    const bool same = !s.keyFound ? value == vanillaValue(mod)
                      : def(mod).kind == modreq::Kind::Bool ? (s.value != 0) == (value != "0")
                                                            : formatValue(s.value) == value;
    if ((found && same) || (!s.iniFound && value == vanillaValue(mod)))
        return true;
    const bool written = found ? ini::set(file, def(mod).section, def(mod).key, value, mod.comment)
                               : writeFresh(src, file, def(mod).section, def(mod).key, value);
    if (!written)
    {
        pf::logError("mods: writing %s failed", file.c_str());
        return false;
    }
    pf::logInfo("mods: %s %s %s = %s in %s%s", src.mod, def(mod).name, def(mod).key,
                value.empty() ? "(removed: vanilla)" : value.c_str(), file.c_str(), found ? "" : " (created, vanilla)");
    return true;
}

bool save(const Mod& mod, const std::string& gameFolder, bool on)
{
    return save(mod, gameFolder, std::string(on ? "1" : "0"));
}

}}
