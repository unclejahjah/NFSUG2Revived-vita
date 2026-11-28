// State of the start-up screen and the settings manager; drawing is in cfgui_draw.cpp.

#include "cfgui.h"
#include "language.h"
#include <platform/regfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ug2 { namespace cfgui {

using pf::Setting;
using pf::SettingType;

namespace
{

const uint64_t RepeatDelayUs = 400000;
const uint64_t RepeatEveryUs = 70000;
const uint32_t Directions[4] = { BtnUp, BtnDown, BtnLeft, BtnRight };

bool belowMin(const Setting& s, const std::string& value)
{
    return ((s.flags & pf::SettingOff) && value == "off") || ((s.flags & pf::SettingAuto) && value == "auto");
}

}

std::string formatModValue(double value)
{
    char text[32];
    snprintf(text, sizeof text, "%g", value);
    return text;
}

std::string resetOf(const ModView& view)
{
    return view.vanillaChoice ? view.reset : view.reset.empty() ? "0" : view.reset;
}

std::string steppedValue(const ModView& view, const std::string& value, int direction, int steps)
{
    const double now = atof(value.c_str());
    if (view.kind == ModKind::Enum)
    {
        const int count = int(view.max - view.min) + 1;
        const int at = int(std::lround(now - view.min));
        const int from = at < 0 || at >= count ? (direction > 0 ? -1 : count) : at;
        return formatModValue(view.min + ((from + direction) % count + count) % count);
    }
    if (view.vanillaChoice && value.empty())
        return direction > 0 ? formatModValue(view.min) : value;
    double next = now + double(direction) * steps * view.step;
    if (view.kind == ModKind::Float)
        next = std::round(next / view.step) * view.step;
    if (view.vanillaChoice && next < view.min)
        return std::string();
    return formatModValue(std::min(view.max, std::max(view.min, next)));
}

std::string shownModValue(const ModView& view, const std::string& value)
{
    if (view.vanillaChoice && value.empty())
        return "Vanilla";
    if (view.kind == ModKind::Bool)
        return value == "0" ? "off" : "on";
    if (view.kind == ModKind::Enum)
    {
        const double at = atof(value.c_str()) - view.min;
        if (at >= 0 && at < double(view.labels.size()) && at == std::floor(at))
            return view.labels[size_t(at)];
    }
    return value;
}

std::string valueOf(const Setting& s, const std::string& cfgText)
{
    std::string raw;
    if (!pf::cfgfile::find(cfgText, s.key, &raw))
        return pf::defaultText(s);
    switch (s.type)
    {
    case SettingType::Bool:
    {
        bool on = false;
        return pf::cfgfile::parseBool(raw.c_str(), &on) ? (on ? "on" : "off") : raw;
    }
    case SettingType::Int:
    {
        int n = 0;
        if (belowMin(s, raw))
            return raw;
        return pf::cfgfile::parseInt(raw.c_str(), &n) ? std::to_string(n) : raw;
    }
    case SettingType::Enum:
    {
        const int index = pf::enumIndex(s, raw.c_str());
        return index >= 0 ? pf::enumName(s, index) : raw;
    }
    case SettingType::String:
        return raw;
    }
    return raw;
}

const char RegistryPath[] = "HKEY_LOCAL_MACHINE\\Software\\EA Games\\Need for Speed Underground 2";

std::string registryValueOf(const Setting& s, const std::string& registryText)
{
    std::string raw;
    if (strcmp(s.key, "language") == 0 && pf::regfile::findString(registryText, RegistryPath, "Language", &raw))
        return ug2::language::optionOf(raw);
    return pf::defaultText(s);
}

bool atDefault(const Setting& s, const std::string& value)
{
    return value == pf::defaultText(s);
}

Configurator::Configurator(const std::string& cfgText, const std::vector<ModView>& mods, const char* version,
                           const std::string& registryText)
    :   m_cfgText(cfgText), m_registryText(registryText), m_version(version ? version : "")
{
    int count = 0;
    const Setting* table = pf::settingsTable(&count);
    for (int g = 0; g < int(pf::SettingGroup::Count); ++g)
    {
        bool heading = false;
        for (int i = 0; i < count; ++i)
        {
            const Setting& s = table[i];
            if (int(s.group) != g || !pf::settingReadInThisBuild(s) || (s.flags & (pf::SettingInGame | pf::SettingCfgOnly)))
                continue;
            if (!heading)
            {
                m_rows.push_back(Row{ nullptr, s.group, std::string(), std::string() });
                heading = true;
            }
            const std::string value = (s.flags & pf::SettingRegistry) ? registryValueOf(s, registryText) : valueOf(s, cfgText);
            m_rows.push_back(Row{ &s, s.group, value, value });
        }
    }
    for (const ModView& view : mods)
    {
        const std::string initial = view.kind == ModKind::Bool ? (view.enabled ? "1" : "0") : view.value;
        m_mods.push_back(Mod{ view, initial, initial });
    }
    rebuild();
    for (int t = 0; t < 2; ++t)
    {
        m_tab = Tab(t);
        m_selection[t] = firstSelectable(0, 1);
    }
    m_tab = Tab::Mods;
}

void Configurator::rebuild()
{
    m_configView.clear();
    int pendingHeading = -1;
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        const Row& row = m_rows[i];
        if (!row.setting)
        {
            pendingHeading = int(i);
            continue;
        }
        const bool dev = row.group == pf::SettingGroup::Developer;
        if (dev && !m_show[int(Tab::Config)])
            continue;
        if (pendingHeading >= 0)
            m_configView.push_back(pendingHeading);
        pendingHeading = -1;
        m_configView.push_back(int(i));
    }
    m_modRows.clear();
    std::vector<std::string> groups;
    for (const Mod& mod : m_mods)
    {
        if (std::find(groups.begin(), groups.end(), mod.view.group) == groups.end())
            groups.push_back(mod.view.group);
    }
    for (const std::string& group : groups)
    {
        bool headed = false;
        for (size_t i = 0; i < m_mods.size(); ++i)
        {
            const ModView& view = m_mods[i].view;
            if (view.group != group || (!view.supported && !m_show[int(Tab::Mods)]))
                continue;
            if (!headed)
                m_modRows.push_back(ModRow{ -1, group });
            headed = true;
            m_modRows.push_back(ModRow{ int(i), group });
        }
    }
}

void Configurator::setConfigFile(const std::string& path, pf::cfgfile::FileState state, const std::string& warning)
{
    m_configPath = path;
    m_configState = state;
    m_configWarning = state == pf::cfgfile::FileState::Unreadable ? warning : std::string();
}

const ModView* Configurator::fileView() const
{
    const int sel = selection();
    if (m_tab != Tab::Mods || m_modRows.empty())
        return nullptr;
    const int row = sel < rowCount() ? sel : rowCount() - 1;
    for (int i = row; i >= 0; --i)
    {
        if (!heading(i))
            return &m_mods[modAt(i)].view;
    }
    return m_mods.empty() ? nullptr : &m_mods[0].view;
}

std::string Configurator::fileLine() const
{
    const ModView* view = fileView();
    const std::string& path = m_tab == Tab::Config ? m_configPath : view ? view->file : std::string();
    const pf::cfgfile::FileState state = m_tab == Tab::Config ? m_configState : view ? view->fileState
                                                                                       : pf::cfgfile::FileState::Readable;
    if (path.empty())
        return std::string();
    if (state == pf::cfgfile::FileState::Missing)
        return path + " (not created yet)";
    if (state == pf::cfgfile::FileState::Unreadable)
        return path + " (unreadable)";
    return path;
}

std::string Configurator::fileWarning() const
{
    if (m_tab == Tab::Config)
        return m_configWarning;
    const ModView* view = fileView();
    return view ? view->warning : std::string();
}

float Configurator::defaultsHold() const
{
    if (!m_selectSince || m_selectDone)
        return 0.0f;
    return std::min(1.0f, float(m_nowUs - m_selectSince) / float(DefaultsHoldUs));
}

int Configurator::defaultsCount() const
{
    int count = 0;
    if (m_tab == Tab::Config)
    {
        for (const Row& row : m_rows)
            count += row.setting && row.current != pf::defaultText(*row.setting);
    }
    else
    {
        for (const Mod& mod : m_mods)
            count += mod.view.supported && mod.current != resetOf(mod.view);
    }
    return count;
}

void Configurator::applyDefaults()
{
    if (m_tab == Tab::Config)
    {
        for (Row& row : m_rows)
            if (row.setting)
                row.current = pf::defaultText(*row.setting);
    }
    else
    {
        for (Mod& mod : m_mods)
            if (mod.view.supported)
                mod.current = resetOf(mod.view);
    }
}

int Configurator::firstSelectable(int from, int direction) const
{
    for (int i = from; i >= 0 && i <= rowCount(); i += direction)
    {
        if (selectable(i))
            return i;
    }
    return rowCount();
}

void Configurator::toggleShow()
{
#ifndef DEV_SETTINGS
    if (m_tab == Tab::Config)
        return;
#endif
    const int sel = selection();
    const bool buttons = onButtons();
    const int item = buttons ? -1 : m_tab == Tab::Config ? m_configView[sel] : modAt(sel);
    m_show[int(m_tab)] = !m_show[int(m_tab)];
    rebuild();
    int& index = m_selection[int(m_tab)];
    if (buttons)
    {
        index = rowCount();
        return;
    }
    for (int i = 0; i < rowCount(); ++i)
    {
        if ((m_tab == Tab::Config ? m_configView[i] : modAt(i)) == item)
        {
            index = i;
            return;
        }
    }
    const int from = std::min(sel, rowCount());
    index = firstSelectable(from, 1);
    if (index == rowCount() && from > 0)
    {
        const int back = firstSelectable(from - 1, -1);
        if (back < rowCount() && selectable(back))
            index = back;
    }
}

void Configurator::begin(uint64_t nowUs, bool splash)
{
    m_startUs = m_nowUs = nowUs;
    m_screen = splash ? Screen::Splash : Screen::Manager;
    m_previous = 0;
    m_ignored = ~0u;
}

int Configurator::rowCount() const
{
    return m_tab == Tab::Config ? int(m_configView.size()) : int(m_modRows.size());
}

bool Configurator::heading(int index) const
{
    return m_tab == Tab::Config ? configRow(index).setting == nullptr : m_modRows[index].mod < 0;
}

bool Configurator::selectable(int index) const
{
    if (index == rowCount())
        return true;
    if (heading(index))
        return false;
    return m_tab == Tab::Config || m_mods[modAt(index)].view.supported;
}

std::string Configurator::selectedKey() const
{
    const int index = selection();
    if (m_tab != Tab::Config || index >= rowCount() || !configRow(index).setting)
        return std::string();
    return configRow(index).setting->key;
}

void Configurator::selectKey(const char* key)
{
    m_tab = Tab::Config;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < rowCount(); ++i)
        {
            if (configRow(i).setting && strcmp(configRow(i).setting->key, key) == 0)
            {
                m_selection[int(Tab::Config)] = i;
                return;
            }
        }
        if (!m_show[int(Tab::Config)])
            toggleShow();
    }
}

std::string Configurator::value(const char* key) const
{
    for (const Row& row : m_rows)
    {
        if (row.setting && strcmp(row.setting->key, key) == 0)
            return row.current;
    }
    return std::string();
}

bool Configurator::changed(const char* key) const
{
    for (const Row& row : m_rows)
    {
        if (row.setting && strcmp(row.setting->key, key) == 0)
            return row.current != row.initial;
    }
    return false;
}

int Configurator::changeCount() const
{
    int count = 0;
    for (const Row& row : m_rows)
        count += row.current != row.initial;
    for (size_t i = 0; i < m_mods.size(); ++i)
        count += modChanged(int(i));
    return count;
}

std::vector<pf::cfgfile::Change> Configurator::configChanges() const
{
    std::vector<pf::cfgfile::Change> changes;
    for (const Row& row : m_rows)
    {
        if (row.setting && !(row.setting->flags & pf::SettingRegistry) && row.current != row.initial)
            changes.push_back({ row.setting->key, row.current, atDefault(*row.setting, row.current) });
    }
    return changes;
}

std::vector<pf::cfgfile::Change> Configurator::registryChanges() const
{
    std::vector<pf::cfgfile::Change> changes;
    for (const Row& row : m_rows)
    {
        if (row.setting && (row.setting->flags & pf::SettingRegistry) && row.current != row.initial)
            changes.push_back({ row.setting->key, row.current, atDefault(*row.setting, row.current) });
    }
    return changes;
}

std::string Configurator::savedRegistry() const
{
    std::string text = m_registryText;
    for (const pf::cfgfile::Change& change : registryChanges())
    {
        if (change.key != "language")
            continue;
        const std::string value = ug2::language::registryValueOf(change.value);
        text = pf::regfile::setString(text, RegistryPath, "Language",
                                      value.empty() ? std::string(ug2::language::DefaultRegistryName) : value);
    }
    return text;
}

std::string Configurator::savedConfig() const
{
    return pf::cfgfile::apply(m_cfgText, configChanges());
}

void Configurator::move(int delta)
{
    int& index = m_selection[int(m_tab)];
    int next = index;
    do
        next += delta;
    while (next >= 0 && next <= rowCount() && !selectable(next));
    if (next >= 0 && next <= rowCount())
        index = next;
}

void Configurator::changeValue(int direction, int steps)
{
    const int index = selection();
    if (index >= rowCount() || steps <= 0)
        return;
    if (m_tab == Tab::Mods)
    {
        Mod& mod = m_mods[modAt(index)];
        if (mod.view.kind != ModKind::Bool)
        {
            mod.current = steppedValue(mod.view, mod.current, direction, steps);
            return;
        }
        if (mod.current == "0" && m_files && mod.view.id >= 0 && mod.view.downloadSpace &&
            !m_files->requirementsMet(mod.view.id))
        {
            m_download = modAt(index);
            m_askDownload = true;
        }
        mod.current = mod.current == "0" ? "1" : "0";
        return;
    }
    Row& row = configRow(index);
    const Setting& s = *row.setting;
    std::string& value = row.current;
    switch (s.type)
    {
    case SettingType::Bool:
    {
        std::vector<std::string> options = { "off", "on" };
        if (s.flags & pf::SettingAuto)
            options.insert(options.begin(), "auto");
        const int count = int(options.size());
        int at = int(std::find(options.begin(), options.end(), value) - options.begin());
        if (at == count)
            at = direction > 0 ? -1 : count;
        value = options[((at + direction) % count + count) % count];
        break;
    }
    case SettingType::Enum:
    {
        const int count = pf::enumCount(s);
        int at = pf::enumIndex(s, value.c_str());
        if (at < 0)
            at = direction > 0 ? -1 : count;
        value = pf::enumName(s, ((at + direction) % count + count) % count);
        break;
    }
    case SettingType::Int:
    {
        const bool canGoBelow = (s.flags & (pf::SettingOff | pf::SettingAuto)) != 0;
        const char* below = (s.flags & pf::SettingOff) ? "off" : "auto";
        long long n;
        if (belowMin(s, value))
        {
            if (direction < 0)
                break;
            n = s.min - s.step;
        }
        else
        {
            int parsed = s.def;
            if (!pf::cfgfile::parseInt(value.c_str(), &parsed))
                parsed = s.def;
            n = parsed;
        }
        n += (long long)direction * steps * s.step;
        if (n < s.min)
        {
            value = canGoBelow ? below : std::to_string(s.min);
            break;
        }
        value = std::to_string(std::min<long long>(n, s.max));
        break;
    }
    case SettingType::String:
        break;
    }
}

void Configurator::resetValue()
{
    const int index = selection();
    if (index >= rowCount())
        return;
    if (m_tab == Tab::Mods)
    {
        Mod& mod = m_mods[modAt(index)];
        if (mod.view.supported)
            mod.current = resetOf(mod.view);
        return;
    }
    Row& row = configRow(index);
    if (row.setting->type != SettingType::String)
        row.current = pf::defaultText(*row.setting);
}

bool Configurator::repeat(uint32_t held, uint32_t button, uint64_t nowUs)
{
    const int i = int(std::find(Directions, Directions + 4, button) - Directions);
    if (!(held & button) || (m_ignored & button))
    {
        m_heldSince[i] = 0;
        return false;
    }
    if (!m_heldSince[i])
    {
        m_heldSince[i] = nowUs;
        m_nextRepeat[i] = nowUs + RepeatDelayUs;
        return true;
    }
    if (nowUs >= m_nextRepeat[i])
    {
        m_nextRepeat[i] += RepeatEveryUs;
        if (m_nextRepeat[i] < nowUs)
            m_nextRepeat[i] = nowUs + RepeatEveryUs;
        return true;
    }
    return false;
}

void Configurator::stick(const Input& input, uint64_t nowUs)
{
    const float dx = float(std::min(127, std::max(-127, input.stickX - 128)));
    const float dy = float(std::min(127, std::max(-127, input.stickY - 128)));
    const float magnitude = std::hypot(dx, dy) / 127.0f;
    if (magnitude <= StickDeadZone)
    {
        m_stickAxis = 0;
        m_stickSteps = 0.0;
        return;
    }
    const float angle = std::atan2(std::fabs(dy), std::fabs(dx)) * 57.29578f;
    const int dominant = std::fabs(dx) > std::fabs(dy) ? 1 : 2;
    if (std::fabs(angle - 45.0f) <= StickDiagonalBand || (m_stickAxis && dominant != m_stickAxis))
    {
        m_stickLastUs = nowUs;
        return;
    }
    if (!m_stickAxis)
    {
        m_stickAxis = dominant;
        m_stickSince = m_stickLastUs = nowUs;
        m_stickSteps = 1.0;
    }
    const double t = double(std::min(1.0f, (magnitude - StickDeadZone) / (1.0f - StickDeadZone)));
    const double held = double(nowUs - m_stickSince) / 1e6;
    const double base = m_stickAxis == 2 ? 4.0 + 16.0 * t : 3.0 + 27.0 * t;
    m_stickSteps += std::min(5000.0, base * (1.0 + 2.0 * held * held)) * double(nowUs - m_stickLastUs) / 1e6;
    m_stickLastUs = nowUs;
    const int steps = int(std::floor(m_stickSteps));
    if (steps <= 0)
        return;
    m_stickSteps -= steps;
    if (m_stickAxis == 2)
    {
        for (int i = 0; i < steps; ++i)
            move(dy < 0 ? -1 : 1);
        return;
    }
    const int index = selection();
    if (m_tab == Tab::Config && index < rowCount() && configRow(index).setting->type == SettingType::Int)
        changeValue(dx < 0 ? -1 : 1, steps);
    if (m_tab == Tab::Mods && index < rowCount() && (m_mods[modAt(index)].view.kind == ModKind::Int ||
                                                     m_mods[modAt(index)].view.kind == ModKind::Float))
        changeValue(dx < 0 ? -1 : 1, steps);
}

Outcome Configurator::update(const Input& input, uint64_t nowUs)
{
    m_nowUs = nowUs;
    const uint32_t held = input.buttons;
    if (m_ignored == ~0u)
        m_ignored = held & (m_screen == Screen::Splash ? ~uint32_t(BtnStart) : ~0u);
    m_ignored &= held;
    const uint32_t pressed = held & ~m_previous & ~m_ignored;
    m_previous = held;
    const bool released = m_touchDown && !input.touch;
    if (input.touch)
    {
        if (!m_touchDown)
        {
            m_touchStartX = input.touchX;
            m_touchStartY = input.touchY;
        }
        m_touchX = input.touchX;
        m_touchY = input.touchY;
    }
    m_touchDown = input.touch;
    auto tapped = [&](const Input&, Rect (*rect)(int)) {
        for (int i = 0; released && i < 2; ++i)
        {
            if (rect(i).contains(m_touchStartX, m_touchStartY) && rect(i).contains(m_touchX, m_touchY))
                return i;
        }
        return -1;
    };
    auto enter = [&](Screen screen) {
        m_screen = screen;
        m_ignored = held;
        m_touchDown = false;
        for (uint64_t& since : m_heldSince)
            since = 0;
    };
    switch (m_screen)
    {
    case Screen::Splash:
        if ((pressed & BtnStart) || (held & BtnStart & ~m_ignored))
        {
            enter(Screen::Manager);
            return Outcome::Running;
        }
        if (pressed || nowUs - m_startUs >= SplashUs)
        {
            enter(Screen::Launch);
            return Outcome::Boot;
        }
        return Outcome::Running;
    case Screen::Manager:
        break;
    case Screen::ConfirmCancel:
    case Screen::ConfirmSave:
    {
        const int tap = tapped(input, choiceRect);
        if ((pressed & BtnCircle) || tap == 0)
            enter(Screen::Manager);
        else if ((pressed & BtnCross) || tap == 1)
        {
            const bool save = m_screen == Screen::ConfirmSave;
            enter(Screen::Launch);
            return save ? Outcome::SaveAndBoot : Outcome::Boot;
        }
        return Outcome::Running;
    }
    case Screen::Launch:
        return Outcome::Running;
    case Screen::ConfirmDownload:
    {
        const int tap = tapped(input, choiceRect);
        if ((pressed & BtnCircle) || tap == 0)
        {
            endDownload(false);
            enter(Screen::Manager);
        }
        else if ((pressed & BtnCross) || tap == 1)
        {
            m_progress = ModFiles::Progress();
            m_stopping = false;
            m_complete = false;
            if (m_files->start(m_mods[m_download].view.id, &m_error))
                enter(Screen::Downloading);
            else
            {
                endDownload(false);
                enter(Screen::DownloadError);
            }
        }
        return Outcome::Running;
    }
    case Screen::Downloading:
    case Screen::ConfirmStop:
    {
        if (m_complete)
        {
            if (pressed || released)
                enter(Screen::Manager);
            return Outcome::Running;
        }
        m_progress = m_files->progress();
        const ModFiles::Phase phase = m_progress.phase;
        if (phase == ModFiles::Phase::Done && m_files->requirementsMet(m_mods[m_download].view.id))
        {
            m_files->finish();
            endDownload(true);
            m_complete = true;
            m_progress.fraction = 1.0f;
            if (m_screen == Screen::ConfirmStop)
                enter(Screen::Downloading);
            return Outcome::Running;
        }
        if (phase == ModFiles::Phase::Done || phase == ModFiles::Phase::Failed || phase == ModFiles::Phase::Cancelled)
        {
            m_files->finish();
            const bool met = phase == ModFiles::Phase::Done && m_files->requirementsMet(m_mods[m_download].view.id);
            if (phase == ModFiles::Phase::Done && !met)
                m_error = "The files are installed but the mod's requirements are still not met.";
            else if (phase == ModFiles::Phase::Failed)
                m_error = m_progress.error;
            endDownload(met);
            enter(phase == ModFiles::Phase::Cancelled || met ? Screen::Manager : Screen::DownloadError);
            return Outcome::Running;
        }
        const int tap = m_screen == Screen::ConfirmStop ? tapped(input, choiceRect) : -1;
        if (m_screen == Screen::Downloading)
        {
            if ((pressed & BtnCircle) && !m_stopping)
                enter(Screen::ConfirmStop);
        }
        else if ((pressed & BtnCircle) || tap == 0)
            enter(Screen::Downloading);
        else if ((pressed & BtnCross) || tap == 1)
        {
            m_files->cancel();
            m_stopping = true;
            enter(Screen::Downloading);
        }
        return Outcome::Running;
    }
    case Screen::DownloadError:
    {
        if ((pressed & (BtnCross | BtnCircle)) || tapped(input, okRect) >= 0)
            enter(Screen::Manager);
        return Outcome::Running;
    }
    }

    const int tap = tapped(input, buttonRect);
    if ((pressed & BtnCircle) || tap == 0)
    {
        enter(Screen::ConfirmCancel);
        return Outcome::Running;
    }
    if ((pressed & BtnStart) || tap == 1)
    {
        enter(Screen::ConfirmSave);
        return Outcome::Running;
    }
    if (pressed & BtnTriangle)
        toggleShow();
    if (!(held & BtnSelect) || (m_ignored & BtnSelect))
    {
        m_selectSince = 0;
        m_selectDone = false;
    }
    else if (!m_selectSince)
        m_selectSince = nowUs;
    else if (!m_selectDone && nowUs - m_selectSince >= DefaultsHoldUs)
    {
        applyDefaults();
        m_selectDone = true;
        m_flashUntil = nowUs + 1200000;
    }
    if (pressed & (BtnL | BtnR))
    {
        m_tab = (pressed & BtnL) ? Tab::Mods : Tab::Config;
        m_selectDone = true;
        return Outcome::Running;
    }
    if (repeat(held, BtnUp, nowUs))
        move(-1);
    if (repeat(held, BtnDown, nowUs))
        move(1);
    const bool left = repeat(held, BtnLeft, nowUs), right = repeat(held, BtnRight, nowUs);
    if (onButtons())
    {
        if (left)
            m_buttonFocus = 0;
        if (right)
            m_buttonFocus = 1;
        if (pressed & BtnCross)
            enter(m_buttonFocus ? Screen::ConfirmSave : Screen::ConfirmCancel);
        else
            stick(input, nowUs);
        return Outcome::Running;
    }
    if (left)
        changeValue(-1, 1);
    if (right)
        changeValue(1, 1);
    if (pressed & BtnCross)
    {
        const int index = selection();
        const bool stepped = m_tab == Tab::Mods ? m_mods[modAt(index)].view.kind == ModKind::Int ||
                                                    m_mods[modAt(index)].view.kind == ModKind::Float
                                                : configRow(index).setting->type == SettingType::Int;
        if (!stepped)
            changeValue(1, 1);
    }
    if (pressed & BtnSquare)
        resetValue();
    if (m_askDownload)
    {
        m_askDownload = false;
        enter(Screen::ConfirmDownload);
        return Outcome::Running;
    }
    stick(input, nowUs);
    return Outcome::Running;
}

void Configurator::endDownload(bool installed)
{
    m_mods[m_download].current = installed ? "1" : "0";
    if (!installed)
        return;
    for (Mod& mod : m_mods)
    {
        if (mod.view.downloadName == m_mods[m_download].view.downloadName && mod.view.id >= 0 &&
            m_files->requirementsMet(mod.view.id))
            mod.view.requirements.clear();
    }
}

namespace
{

std::string megabytes(uint64_t bytes, bool roundUp)
{
    const uint64_t mb = 1000000;
    if (roundUp)
        return std::to_string((bytes + mb - 1) / mb) + " MB";
    char text[32];
    snprintf(text, sizeof(text), "%.1f MB", double(bytes) / double(mb));
    return text;
}

}

std::string Configurator::popupText() const
{
    switch (m_screen)
    {
    case Screen::ConfirmDownload:
    {
        const ModView& view = m_mods[m_download].view;
        return "This mod requires files for " + view.downloadName + " to be downloaded from the internet. Total free "
               "space required: " + megabytes(view.downloadSpace, true) + ". Begin download?";
    }
    case Screen::ConfirmStop:
        return "Stop the download? The files fetched so far will be removed.";
    case Screen::DownloadError:
        return m_error;
    default:
        return std::string();
    }
}

std::string Configurator::progressText() const
{
    if (m_complete)
        return "Install complete";
    if (m_stopping)
        return "Cancelling...";
    switch (m_progress.phase)
    {
    case ModFiles::Phase::Download:
        return megabytes(m_progress.downloaded, false) + " / " + megabytes(m_progress.total, false);
    case ModFiles::Phase::Unpack:
        return "Unpacking...";
    default:
        return "Installing...";
    }
}

}}
