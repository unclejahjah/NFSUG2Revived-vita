#include <lib/registry.h>
#include <lib/guest.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>

namespace win32 { namespace registry
{

struct Value
{
    std::string             name;
    x86::reg32              type;
    std::vector<x86::reg8>  data;
    bool                    temporary = false;      // set for this run only: the file keeps kept, if there is one
    bool                    keptExists = false;
    x86::reg32              keptType = 0;
    std::vector<x86::reg8>  kept;
};

struct Key
{
    std::string                         name;
    Key*                                parent;
    std::vector<std::unique_ptr<Key>>   children;
    std::vector<Value>                  values;
};

namespace
{

const x86::reg32 ERROR_SUCCESS = 0;
const x86::reg32 ERROR_FILE_NOT_FOUND = 2;
const x86::reg32 ERROR_MORE_DATA = 234;

struct Root
{
    x86::reg32  handle;
    const char* name;
};

const Root s_rootNames[] =
{
    { HKEY_CLASSES_ROOT,   "HKEY_CLASSES_ROOT" },
    { HKEY_CURRENT_USER,   "HKEY_CURRENT_USER" },
    { HKEY_LOCAL_MACHINE,  "HKEY_LOCAL_MACHINE" },
    { HKEY_USERS,          "HKEY_USERS" },
    { HKEY_CURRENT_CONFIG, "HKEY_CURRENT_CONFIG" },
};
const size_t s_rootCount = sizeof(s_rootNames) / sizeof(s_rootNames[0]);

Key s_roots[s_rootCount];
pf::LwMutex s_mutex("registry", true);
std::string s_path;
bool s_existed = false;                 // registry.txt was there at load

Key* child(Key* parent, const std::string& name, bool create)
{
    for (const std::unique_ptr<Key>& k : parent->children)
    {
        if (strcasecmp(k->name.c_str(), name.c_str()) == 0)
            return k.get();
    }
    if (!create)
        return nullptr;
    parent->children.emplace_back(new Key{name, parent, {}, {}});
    return parent->children.back().get();
}

Value* findValue(Key* key, const std::string& name)
{
    for (Value& v : key->values)
    {
        if (strcasecmp(v.name.c_str(), name.c_str()) == 0)
            return &v;
    }
    return nullptr;
}

std::string pathOf(const Key* key)
{
    std::string path;
    for (; key->parent; key = key->parent)
        path = "\\" + key->name + path;
    return key->name + path;
}

// One section per key with values: "[path]", then "name<TAB>type<TAB>hex bytes" lines.
void writeKey(FILE* file, const Key* key)
{
    if (!key->values.empty())
    {
        fprintf(file, "[%s]\n", pathOf(key).c_str());
        for (const Value& v : key->values)
        {
            if (v.temporary && !v.keptExists)
                continue;
            fprintf(file, "%s\t%u\t", v.name.c_str(), unsigned(v.temporary ? v.keptType : v.type));
            for (x86::reg8 b : v.temporary ? v.kept : v.data)
                fprintf(file, "%02x", b);
            fputc('\n', file);
        }
    }
    for (const std::unique_ptr<Key>& k : key->children)
        writeKey(file, k.get());
}

void save()
{
    if (s_path.empty())
        return;
    FILE* file = fopen(s_path.c_str(), "w");
    if (!file)
    {
        pf::logError("registry: cannot write %s", s_path.c_str());
        return;
    }
    for (const Key& root : s_roots)
        writeKey(file, &root);
    fclose(file);
}

void store(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    Value* v = findValue(key, name);
    if (!v)
    {
        key->values.push_back(Value{name, type, {}, false, false, 0, {}});
        v = &key->values.back();
    }
    v->type = type;
    v->temporary = false;
    const x86::reg8* bytes = static_cast<const x86::reg8*>(data);
    v->data.assign(bytes, bytes + size);
}

}

static void loadFile(const char* path);

namespace
{

struct Setting
{
    const char* name;
    x86::reg32  value;
};

// Graphics options of SPEED2.EXE (read at 0x5BEA20), set on first launch.
const Setting s_settings[] = {
    { "VERSION", 1 },
    { "SIZE", 0x78 },
    { "g_CarEnvironmentMapEnable", 0 },         // no car cube map renders
    { "g_CarEnvironmentMapUpdateData", 0 },
    { "g_CarShadowEnable", 1 },                 // the basic shadow; 2 adds a shadow pass
    { "g_CarHeadlightEnable", 0 },              // no projected headlights
    { "g_CarLightingEnable", 0 },               // unused by the game
    { "g_CarDamageEnable", 0 },                 // unused by the game
    { "g_CrowdEnable", 0 },
    { "g_RoadReflectionEnable", 0 },            // no reflection render of the world
    { "g_FogEnable", 0 },                       // a shader term; hides the draw distance
    { "g_MotionBlurEnable", 0 },
    { "g_LightStreaksEnable", 0 },
    { "g_LightGlowEnable", 0 },
    { "g_AnimatedTextureEnable", 1 },           // the game's default
    { "g_ParticleSystemEnable", 0 },
    { "g_DepthOfFieldEnable", 0 },
    { "g_WorldLodLevel", 0 },                   // low scenery detail (0x5BE1C0)
    { "g_CarLodLevel", 0 },                     // low-detail car models (0x5D2850)
    { "g_OverBrightEnable", 0 },
    { "g_BleachByPassEnable", 0 },
    { "g_TintingEnable", 0 },
    { "g_FSAALevel", 0 },
    { "g_HorizonFogEnable", 0 },                // a full-screen pass
    { "g_RainEnable", 0 },
    { "g_TextureFiltering", 1 },                // bilinear (0x5BE560)
    { "g_RacingResolution", 0 },                // 640x480
    { "g_PerformanceLevel", 0 },                // mixes audio at 22.05 kHz (0x47ABA9)
    { "g_VSyncOn", 0 },
};

// The defaults a new install starts with (without gfx_level).
const Setting s_vitaSettings[] = {
    { "g_CarEnvironmentMapEnable", 2 },
    { "g_CarEnvironmentMapUpdateData", 1 },
    { "g_CarHeadlightEnable", 1 },
    { "g_CrowdEnable", 1 },
    { "g_FogEnable", 1 },
    { "g_MotionBlurEnable", 1 },
    { "g_LightStreaksEnable", 1 },
    { "g_LightGlowEnable", 1 },
    { "g_ParticleSystemEnable", 1 },
    { "g_CarLodLevel", 1 },
    { "g_TintingEnable", 1 },
    { "g_RainEnable", 1 },
    { "g_PerformanceLevel", 2 },
    { "g_RoadReflectionEnable", 1 },
    { "g_WorldLodLevel", 2 },
};

// gfx_level=1..5 (measurement): the game's preset 0x5BE870 for that Level of Detail; the last match at or below wins.
struct LevelSetting
{
    const char* name;
    int         from;
    x86::reg32  value;
};
const LevelSetting s_levelSettings[] = {
    { "g_CarShadowEnable", 1, 0 },
    { "g_CarShadowEnable", 2, 2 }, { "g_CarHeadlightEnable", 2, 1 }, { "g_CrowdEnable", 2, 1 },
    { "g_LightGlowEnable", 2, 1 }, { "g_WorldLodLevel", 2, 1 }, { "g_TintingEnable", 2, 1 },
    { "g_CarEnvironmentMapEnable", 3, 1 }, { "g_CarEnvironmentMapUpdateData", 3, 1 },
    { "g_RoadReflectionEnable", 3, 2 }, { "g_FogEnable", 3, 1 }, { "g_ParticleSystemEnable", 3, 1 },
    { "g_CarLodLevel", 3, 1 }, { "g_MotionBlurEnable", 3, 1 }, { "g_HorizonFogEnable", 3, 1 },
    { "g_WorldLodLevel", 3, 2 }, { "g_DepthOfFieldEnable", 3, 1 }, { "g_OverBrightEnable", 3, 1 },
    { "g_LightStreaksEnable", 3, 1 },
    { "g_CarEnvironmentMapEnable", 4, 2 }, { "g_RainEnable", 4, 1 }, { "g_RoadReflectionEnable", 4, 3 },
    { "g_BleachByPassEnable", 4, 1 }, { "g_FSAALevel", 4, 1 },
    { "g_TextureFiltering", 5, 2 }, { "g_CarEnvironmentMapEnable", 5, 3 },
};

// gfx_set=name:value,...: development overrides on top of the Vita values.
bool overrideValue(const char* list, const char* name, x86::reg32* value)
{
    const size_t length = strlen(name);
    for (const char* p = list; *p;)
    {
        if (strncmp(p, name, length) == 0 && p[length] == ':')
        {
            *value = x86::reg32(strtol(p + length + 1, nullptr, 0));
            return true;
        }
        const char* comma = strchr(p, ',');
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

x86::reg32 settingValue(const Setting& s, int level, const char* overrides)
{
    x86::reg32 value = s.value;
    if (overrideValue(overrides, s.name, &value))
        return value;
    if (level && strcmp(s.name, "g_PerformanceLevel") == 0)
        return x86::reg32(level);
    for (const LevelSetting& l : s_levelSettings)
        if (l.from <= level && strcmp(l.name, s.name) == 0)
            value = l.value;
    if (!level)
        for (const Setting& v : s_vitaSettings)
            if (strcmp(v.name, s.name) == 0)
                return v.value;
    return value;
}

}

// The values last applied, "vita<level>;<gfx_set>"; the game ignores unknown values.
const char PresetMarker[] = "NFSU2VITA graphics preset";

// Install values the game reads; the Vita graphics values go in only on first launch.
static void seedDefaults()
{
    Key* game = find(root(HKEY_LOCAL_MACHINE), "Software\\EA Games\\Need for Speed Underground 2", true);
    static const char installDir[] = "C:\\";
    static const char language[] = "English US";
    setDefault(game, "Install Dir", REG_SZ, installDir, sizeof(installDir));
    setDefault(game, "Language", REG_SZ, language, sizeof(language));
    // Without NotFirstTime the game replaces the settings with its detected preset (0x5CEC90).
    const x86::reg32 notFirstTime = 1;
    setDefault(game, "NotFirstTime", REG_DWORD, &notFirstTime, sizeof(notFirstTime));
    const char* overrides = NFSU_DEV_STRING("gfx_set", "");
    const int level = std::min(5, std::max(0, NFSU_DEV_INT("gfx_level", 0)));
    const std::string tail = (level ? std::to_string(level) : "") + ";" + overrides;
    const std::string applied = "vita" + tail;
    {
        pf::LwLockGuard guard(s_mutex);
        const Value* last = findValue(game, PresetMarker);
        const std::string saved = last ? std::string(last->data.begin(), last->data.end()) : std::string();
        const size_t name = saved.find_first_not_of("abcdefghijklmnopqrstuvwxyz");
        const bool changed = last && name != std::string::npos && saved.substr(name) != tail + '\0';
        const bool firstLaunch = !s_existed || !findValue(game, "VERSION");
        const bool apply = firstLaunch || changed;
        for (const Setting& s : s_settings)
        {
            const x86::reg32 value = settingValue(s, level, overrides);
            if (apply || !findValue(game, s.name))
                store(game, s.name, REG_DWORD, &value, sizeof(value));
        }
        if (apply || !last)
            store(game, PresetMarker, REG_SZ, applied.c_str(), x86::reg32(applied.size() + 1));
        save();
        pf::logInfo("registry: graphics level %d (gfx_level)%s%s: %s", level, *overrides ? ", overrides " : "", overrides,
                    apply ? (firstLaunch ? "Vita values written, first launch" : "applied over the saved settings")
                          : "the saved settings (Display menu) kept");
    }
    // ~MHz spares the EA clock (0x6F5E41) its one-second busy-wait measurement.
    const x86::reg32 mhz = x86::TimestampMhz;
    Key* cpu = find(root(HKEY_LOCAL_MACHINE), "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    setDefault(cpu, "~MHz", REG_DWORD, &mhz, sizeof(mhz));
}

void load(const char* path)
{
    loadFile(path);
    seedDefaults();
}

static void loadFile(const char* path)
{
    pf::LwLockGuard guard(s_mutex);
    for (size_t i = 0; i < s_rootCount; ++i)
    {
        s_roots[i].name = s_rootNames[i].name;
        s_roots[i].parent = nullptr;
    }
    s_path = path;
    FILE* file = fopen(path, "r");
    s_existed = file != nullptr;
    if (!file)
        return;
    Key* key = nullptr;
    char line[4096];
    while (fgets(line, sizeof(line), file))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[')
        {
            char* end = strchr(line, ']');
            if (end)
                *end = 0;
            char* rest = strchr(line + 1, '\\');
            if (rest)
                *rest++ = 0;
            key = nullptr;
            for (size_t i = 0; i < s_rootCount; ++i)
            {
                if (strcmp(s_rootNames[i].name, line + 1) == 0)
                    key = find(&s_roots[i], rest ? rest : "", true);
            }
            continue;
        }
        char* type = strchr(line, '\t');
        char* hex = type ? strchr(type + 1, '\t') : nullptr;
        if (!key || !hex)
            continue;
        *type++ = 0;
        *hex++ = 0;
        std::vector<x86::reg8> data;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        {
            const char pair[3] = { hex[i], hex[i + 1], 0 };
            data.push_back(x86::reg8(strtoul(pair, nullptr, 16)));
        }
        store(key, line, x86::reg32(strtoul(type, nullptr, 10)), data.data(), x86::reg32(data.size()));
    }
    fclose(file);
    pf::logInfo("registry: loaded %s", path);
}

Key* root(x86::reg32 handle)
{
    for (size_t i = 0; i < s_rootCount; ++i)
    {
        if (s_rootNames[i].handle == handle)
            return &s_roots[i];
    }
    return nullptr;
}

Key* find(Key* parent, const char* path, bool create)
{
    pf::LwLockGuard guard(s_mutex);
    Key* key = parent;
    while (key && path && *path)
    {
        const char* end = strchr(path, '\\');
        const std::string name = end ? std::string(path, end) : std::string(path);
        if (!name.empty())
            key = child(key, name, create);
        path = end ? end + 1 : nullptr;
    }
    return key;
}

void setValue(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    store(key, name, type, data, size);
    save();
}

void setOverride(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    Value* v = findValue(key, name);
    if (!v)
    {
        key->values.push_back(Value{name, type, {}, false, false, 0, {}});
        v = &key->values.back();
    }
    else if (!v->temporary)
    {
        v->kept = v->data;
        v->keptType = v->type;
        v->keptExists = true;
    }
    v->temporary = true;
    v->type = type;
    const x86::reg8* bytes = static_cast<const x86::reg8*>(data);
    v->data.assign(bytes, bytes + size);
}

void setDefault(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    if (findValue(key, name))
        return;
    store(key, name, type, data, size);
    save();
}

// Without data only the type and size are returned; data that does not fit gives ERROR_MORE_DATA with the size
// needed.
x86::reg32 query(Key* key, const std::string& name, x86::reg32* type, void* data, x86::reg32* size)
{
    pf::LwLockGuard guard(s_mutex);
    const Value* v = findValue(key, name);
    if (!v)
        return ERROR_FILE_NOT_FOUND;
    if (type)
        *type = v->type;
    if (!size)
        return ERROR_SUCCESS;
    const x86::reg32 room = *size;
    *size = x86::reg32(v->data.size());
    if (!data)
        return ERROR_SUCCESS;
    if (room < v->data.size())
        return ERROR_MORE_DATA;
    memcpy(data, v->data.data(), v->data.size());
    return ERROR_SUCCESS;
}

}}
