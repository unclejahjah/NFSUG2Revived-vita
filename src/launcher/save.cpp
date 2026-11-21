// The manager's Save: the cfg and the mods' ini files.

#include "save.h"
#include "mods_edit.h"
#include <platform/log.h>
#include <cstdio>
#include <set>
#include <sys/stat.h>

namespace ug2 { namespace launcher {

void save(const cfgui::Configurator& ui, const std::string& configPath, pf::cfgfile::FileState configState,
          const std::string& gameFolder)
{
    const bool unreadable = configState == pf::cfgfile::FileState::Unreadable;
    if ((unreadable || !ui.configChanges().empty()) && !pf::cfgfile::writeFile(configPath.c_str(), ui.savedConfig()))
        pf::logError("launcher: writing %s failed", configPath.c_str());
    std::set<std::string> rewritten;
    for (int i = 0; i < ui.modCount(); ++i)
    {
        const cfgui::ModView& view = ui.modView(i);
        if (view.fileState != pf::cfgfile::FileState::Unreadable || rewritten.count(view.file))
            continue;
        rewritten.insert(view.file);
        remove(view.file.c_str());
        mkdir(view.file.substr(0, view.file.rfind('/')).c_str(), 0777);
        for (int k = 0; k < ui.modCount(); ++k)
        {
            const cfgui::ModView& feature = ui.modView(k);
            if (feature.file != view.file || feature.id < 0)
                continue;
            const mods::Mod& mod = mods::get(mods::Id(feature.id));
            if (!mods::ini::set(view.file, mods::def(mod).section, mods::def(mod).key, ui.modEnabled(k) ? "1" : "0", mod.comment))
                pf::logError("launcher: writing %s failed", view.file.c_str());
        }
        pf::logInfo("launcher: %s was unreadable and is written fresh", view.file.c_str());
    }
    for (int i = 0; i < ui.modCount(); ++i)
    {
        const cfgui::ModView& view = ui.modView(i);
        if (view.id >= 0 && ui.modChanged(i) && !rewritten.count(view.file))
            mods::save(mods::get(mods::Id(view.id)), gameFolder, ui.modEnabled(i));
    }
    pf::logInfo("launcher: saved %d changes", ui.changeCount());
}

}}
