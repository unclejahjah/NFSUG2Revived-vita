#ifndef PF_SWITCHES_H_
#define PF_SWITCHES_H_

// Build switches NATIVE_FUNCTIONS, AUDIO_FAST_PATHS and RENDER_FAST_PATHS (0 or 1) fix each choice at compile time;
// with DEV_SETTINGS the settings file decides, key by key, and the build switch is only the default.

#include <platform/cached.h>
#include <platform/config.h>

#ifdef DEV_SETTINGS
#define NFSU_SWITCH(key, on) (pf::Config::getBool(key, (on) != 0))
#define NFSU_SWITCH_CACHED(key, on) (pf::cachedStatic([] { return pf::Config::getBool(key, (on) != 0); }))
#else
#define NFSU_SWITCH(key, on) ((on) != 0)
#define NFSU_SWITCH_CACHED(key, on) ((on) != 0)
#endif

// Developer settings (VAR and FIELD: a variable that is a constant in release; SET: its assignment): the cfg decides in a DEV_SETTINGS build; a release build has the default as a constant.
#ifdef DEV_SETTINGS
#define NFSU_DEV_BOOL(key, def) (pf::Config::getBool(key, (def)))
#define NFSU_DEV_INT(key, def) (pf::Config::getInt(key, (def)))
#define NFSU_DEV_STRING(key, def) (pf::Config::getString(key, (def)))
#define NFSU_DEV_CHOICE(key, names, count, def) (pf::Config::getChoice(key, (names), (count), (def)))
#define NFSU_DEV_BOOL_CACHED(key, def) (pf::cachedStatic([] { return pf::Config::getBool(key, (def)); }))
#define NFSU_DEV_INT_CACHED(key, def) (pf::cachedStatic([] { return pf::Config::getInt(key, (def)); }))
#define NFSU_DEV_STRING_CACHED(key, def) (pf::cachedStatic([] { return pf::Config::getString(key, (def)); }))
#define NFSU_DEV_VARC(type, name, init, constant) static type name = (init)
#define NFSU_DEV_VAR(type, name, def) NFSU_DEV_VARC(type, name, def, def)
#define NFSU_DEV_FIELD(type, name, def) type name = (def)
#define NFSU_DEV_SET(var, expr) ((void)((var) = (expr)))
#else
#define NFSU_DEV_BOOL(key, def) (def)
#define NFSU_DEV_INT(key, def) (def)
#define NFSU_DEV_STRING(key, def) (def)
#define NFSU_DEV_CHOICE(key, names, count, def) (def)
#define NFSU_DEV_BOOL_CACHED(key, def) (def)
#define NFSU_DEV_INT_CACHED(key, def) (def)
#define NFSU_DEV_STRING_CACHED(key, def) (def)
#define NFSU_DEV_VARC(type, name, init, constant) static constexpr type name = (constant)
#define NFSU_DEV_VAR(type, name, def) NFSU_DEV_VARC(type, name, def, def)
#define NFSU_DEV_FIELD(type, name, def) static constexpr type name = (def)
#define NFSU_DEV_SET(var, expr) ((void)0)
#endif

#define NFSU_NATIVE(key) NFSU_SWITCH_CACHED(key, NATIVE_FUNCTIONS)
#define NFSU_NATIVE_SETTING(key) NFSU_SWITCH(key, NATIVE_FUNCTIONS)
#define NFSU_AUDIO_FAST(key) NFSU_SWITCH(key, AUDIO_FAST_PATHS)
#define NFSU_RENDER_FAST(key) NFSU_SWITCH(key, RENDER_FAST_PATHS)
#define NFSU_RENDER_FAST_CACHED(key) NFSU_SWITCH_CACHED(key, RENDER_FAST_PATHS)

#endif /* !PF_SWITCHES_H_ */
