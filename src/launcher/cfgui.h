#ifndef UG2_CFGUI_H_
#define UG2_CFGUI_H_

// The start-up screen and the settings manager: input, state and drawing into a plain pixel buffer, with no
// platform calls, so the host tests can drive and render it.

#include <platform/cfgfile.h>
#include <platform/settings.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace cfgui {

enum Button : uint32_t
{
    BtnUp = 1, BtnDown = 2, BtnLeft = 4, BtnRight = 8,
    BtnCross = 16, BtnCircle = 32, BtnSquare = 64, BtnTriangle = 128,
    BtnL = 256, BtnR = 512, BtnStart = 1024, BtnSelect = 2048,
};

struct Input
{
    uint32_t buttons = 0;
    int      stickX = 128;      // left stick, 0..255
    int      stickY = 128;
    bool     touch = false;     // a finger on the front screen, at touchX, touchY in 960x544
    int      touchX = 0;
    int      touchY = 0;
};

struct Rect
{
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
Rect buttonRect(int which);     // 0 Cancel, 1 Save
Rect choiceRect(int which);     // in a pop-up: 0 Go Back, 1 OK

// 0xAABBGGRR: R, G, B, A in memory, the Vita display's A8B8G8R8.
struct Canvas
{
    uint32_t* pixels;
    int       width;
    int       height;
    int       stride;
};

// An alpha bitmap of one character: bits is 8, or 4 with two pixels a byte, the left one high.
struct Glyph
{
    const uint8_t* alpha;
    int pitch;
    int bits;
    int width, height;
    int left, top;          // from the pen; top is above the baseline
    int advance64;
};

enum FontSize { FontSmall, FontNormal, FontLarge, FontSizes };

class Font
{
public:
    virtual ~Font() {}
    virtual bool glyph(FontSize size, unsigned code, Glyph* out) const = 0;
    virtual int ascent(FontSize size) const = 0;
};

// Noto Sans from cfgui_atlas.h: host font, Vita fallback.
const Font& bundledFont();
// Ascent aligning capHeight capitals with the reference font's.
int centredAscent(const Font& reference, FontSize size, int capHeight);
// The Vita's system font, or null (always on the host).
const Font* openSystemFont();
void closeSystemFont();

const int Width = 960;
const int Height = 544;
const uint64_t SplashUs = 3000000;
const uint64_t DefaultsHoldUs = 3000000;    // Select held this long sets the tab to its defaults

// One mod the Mods tab offers.
struct ModView
{
    std::string group;                  // the heading: source mod, version and author
    std::string name;
    std::vector<std::string> details;   // the description panel, paragraph by paragraph
    std::string requirements;           // unmet: the setting's requirementsString; the value stays editable
    bool enabled = false;               // as the ini says
    bool supported = true;              // false: a key of the mod's ini the port does not implement yet
    std::string value;                  // an unsupported key's value, shown greyed
    int id = -1;                        // the registry feature (mods::Id) of a supported key
    std::string file;                   // full path of the group's ini
    pf::cfgfile::FileState fileState = pf::cfgfile::FileState::Missing;
    std::string warning;                // shown while the group's ini is unreadable
};

enum class Screen { Splash, Manager, ConfirmCancel, ConfirmSave, Launch };   // Launch: nothing more to show
enum class Tab { Mods, Config };
enum class Outcome { Running, Boot, SaveAndBoot };

class Configurator
{
public:
    Configurator(const std::string& cfgText, const std::vector<ModView>& mods, const char* version);

    // Starts the splash timer; splash=false opens the manager directly.
    void begin(uint64_t nowUs, bool splash = true);
    Outcome update(const Input& input, uint64_t nowUs);
    void draw(Canvas& canvas, const Font& font) const;

    Screen screen() const { return m_screen; }
    Tab tab() const { return m_tab; }
    int selection() const { return m_selection[int(m_tab)]; }
    bool onButtons() const { return selection() == rowCount(); }
    int buttonFocus() const { return m_buttonFocus; }      // 0 Cancel, 1 Save
    std::string selectedKey() const;

    // Value of key as it would be saved.
    std::string value(const char* key) const;
    bool changed(const char* key) const;
    int changeCount() const;
    std::vector<pf::cfgfile::Change> configChanges() const;
    std::string savedConfig() const;                        // the cfg text with the changes applied
    int modAt(int row) const { return m_modRows[row].mod; }      // Mods tab row to mod index, -1 for a heading
    int modCount() const { return int(m_mods.size()); }
    const ModView& modView(int index) const { return m_mods[index].view; }
    // Unsupported mod keys on the Mods tab, dev settings on the Config tab; off at start.
    bool showingAll(Tab tab) const { return m_show[int(tab)]; }
    bool modEnabled(int index) const { return m_mods[index].current; }
    bool modChanged(int index) const { return m_mods[index].current != m_mods[index].view.enabled; }

    void selectKey(const char* key);                        // tests and screenshots; shows dev settings if needed
    void setConfigFile(const std::string& path, pf::cfgfile::FileState state, const std::string& warning);
    // The file the tab edits, with "(not created yet)" or "(unreadable)"; empty when unknown.
    std::string fileLine() const;
    std::string fileWarning() const;                        // the tab's unreadable-file message, or empty
    int defaultsCount() const;                              // values Select's reset would change on this tab
    float defaultsHold() const;                             // 0..1 of the Select hold; 0 when not held

    // Where the hint row ends, and the file line right-justified on the line above it, as drawn with font.
    struct BottomRow
    {
        int hintsEnd;
        int pathX;
        int pathY;
        std::string path;
    };
    BottomRow bottomRow(const Font& font) const;

    // The Mods tab's description panel for mod (0..modCount()-1), as drawn with font.
    struct PanelLine
    {
        std::string text;
        uint32_t color;
        int indent;
    };
    std::vector<PanelLine> modPanel(const Font& font, int mod) const;
    static uint32_t requirementColor();     // the orange of unmet requirements
    static int lineHeight();                // of the Mods tab's description panel

    struct Painter;

private:
    struct Row
    {
        const pf::Setting* setting;     // null for a group heading
        pf::SettingGroup group;
        std::string initial;
        std::string current;
    };
    struct Mod
    {
        ModView view;
        bool current;
    };

    int rowCount() const;
    Row& configRow(int index) { return m_rows[m_configView[index]]; }
    const Row& configRow(int index) const { return m_rows[m_configView[index]]; }
    bool heading(int index) const;
    void rebuild();
    void toggleShow();
    void applyDefaults();
    const ModView* fileView() const;
    int firstSelectable(int from, int direction) const;
    bool selectable(int index) const;
    void move(int delta);
    void changeValue(int direction, int steps);
    void resetValue();
    void stick(const Input& input, uint64_t nowUs);
    bool repeat(uint32_t held, uint32_t button, uint64_t nowUs);

    int drawHints(Painter& p, bool draw) const;     // returns where the hints end
    void drawSplash(Painter& p) const;
    void drawManager(Painter& p) const;
    void drawConfigRows(Painter& p) const;
    void drawModRows(Painter& p) const;
    std::vector<PanelLine> panelLines(const Painter& p, const Mod* mod, bool buttons) const;
    void drawPopup(Painter& p, const char* text) const;

    std::string m_cfgText;
    std::string m_configPath;
    pf::cfgfile::FileState m_configState = pf::cfgfile::FileState::Readable;
    std::string m_configWarning;
    std::string m_version;
    std::vector<Row> m_rows;
    std::vector<int> m_configView;      // rows of m_rows the Config tab shows
    std::vector<Mod> m_mods;
    struct ModRow
    {
        int mod;            // -1 for a heading
        std::string group;
    };
    std::vector<ModRow> m_modRows;
    Screen m_screen = Screen::Splash;
    Tab m_tab = Tab::Mods;
    int m_selection[2] = { 0, 0 };
    int m_buttonFocus = 0;
    bool m_show[2] = { false, false };
    uint64_t m_selectSince = 0;         // 0 while Select is up
    bool m_selectDone = false;          // fired or cancelled: nothing more until Select is released
    uint64_t m_flashUntil = 0;          // "Set to default" shows until then
    bool m_touchDown = false;
    int m_touchStartX = 0, m_touchStartY = 0, m_touchX = 0, m_touchY = 0;
    uint64_t m_startUs = 0;
    uint64_t m_nowUs = 0;
    uint32_t m_previous = 0;
    uint32_t m_ignored = 0;             // held when the screen appeared; counts only once released
    uint64_t m_heldSince[4] = {};       // up, down, left, right
    uint64_t m_nextRepeat[4] = {};
    int m_stickAxis = 0;                // 0 inside the dead zone, 1 horizontal, 2 vertical: the push's axis
    uint64_t m_stickSince = 0;
    uint64_t m_stickLastUs = 0;
    double m_stickSteps = 0.0;
};

// A few centred lines on an empty screen: the launcher's error message.
void drawMessage(Canvas& canvas, const Font& font, const std::vector<std::string>& lines);

// Shared with the runner and the tests.
std::string valueOf(const pf::Setting& setting, const std::string& cfgText);
bool atDefault(const pf::Setting& setting, const std::string& value);
const float StickDeadZone = 0.5f;       // of full deflection, on the stick vector
const float StickDiagonalBand = 4.5f;   // degrees either side of a diagonal where the stick does nothing

}}

#endif
