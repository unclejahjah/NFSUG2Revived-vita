// Texts the port added that the game's language files lack, in the game's languages by their English wording.

#include "translations.h"
#include <cstring>

namespace ug2 { namespace translations {

// Wording follows the game's own: Steer, Brake / Reverse, Left, Right, Customize and Sensitivity in its language files.
const Entry Table[] =
{
    { "Steer Left", { "Direction gauche", "Links lenken", "Sterzo a sinistra", "Girar a la izquierda",
                      "Naar links sturen", "Styr vänster", "Styr til venstre" } },
    { "Steer Right", { "Direction droite", "Rechts lenken", "Sterzo a destra", "Girar a la derecha",
                       "Naar rechts sturen", "Styr höger", "Styr til højre" } },
    { "Free Look", { "Regard libre", "Freie Kamera", "Visuale libera", "Vista libre", "Vrij rondkijken", "Fri blick",
                     "Frit udsyn" } },
    { "Brake", { "Freiner", "Bremsen", "Freno", "Frenar", "Remmen", "Bromsa", "Brems" } },
    { "Left Dead Zone", { "Zone morte stick gauche", "Tote Zone linker Stick", "Zona morta levetta sinistra",
                          "Zona muerta joystick izquierdo", "Dode zone linkerstick", "Dödzon vänster spak",
                          "Dødzone venstre pind" } },
    { "Left Outer Limit", { "Limite extérieure stick gauche", "Äußere Grenze linker Stick",
                            "Limite esterno levetta sinistra", "Límite exterior joystick izquierdo",
                            "Buitengrens linkerstick", "Yttre gräns vänster spak", "Ydre grænse venstre pind" } },
    { "Left Sensitivity", { "Sensibilité stick gauche", "Empfindlichkeit linker Stick",
                            "Sensibilità levetta sinistra", "Sensibilidad joystick izquierdo",
                            "Gevoeligheid linkerstick", "Känslighet vänster spak", "Følsomhed venstre pind" } },
    { "Right Dead Zone", { "Zone morte stick droit", "Tote Zone rechter Stick", "Zona morta levetta destra",
                           "Zona muerta joystick derecho", "Dode zone rechterstick", "Dödzon höger spak",
                           "Dødzone højre pind" } },
    { "Right Outer Limit", { "Limite extérieure stick droit", "Äußere Grenze rechter Stick",
                             "Limite esterno levetta destra", "Límite exterior joystick derecho",
                             "Buitengrens rechterstick", "Yttre gräns höger spak", "Ydre grænse højre pind" } },
    { "Right Sensitivity", { "Sensibilité stick droit", "Empfindlichkeit rechter Stick", "Sensibilità levetta destra",
                             "Sensibilidad joystick derecho", "Gevoeligheid rechterstick", "Känslighet höger spak",
                             "Følsomhed højre pind" } },
    { "Custom", { "Personnalisé", "Benutzerdefiniert", "Personalizzato", "Personalizado", "Aangepast", "Anpassad",
                  "Tilpasset" } },
    { "Touchscreen", { "Écran tactile", "Touchscreen", "Touchscreen", "Pantalla táctil", "Touchscreen", "Pekskärm",
                       "Berøringsskærm" } },
};
const int TableCount = int(sizeof(Table) / sizeof(Table[0]));

namespace
{

int s_language = 0;

std::string latin1(const char* utf8)
{
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p)
    {
        if (*p < 0x80)
            out += char(*p);
        else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80)
        {
            const unsigned code = unsigned(*p & 0x1f) << 6 | (p[1] & 0x3f);
            out += code <= 0xff ? char(code) : '?';
            ++p;
        }
        else
            out += '?';
    }
    return out;
}

}

void setLanguage(int language)
{
    s_language = language;
}

int language()
{
    return s_language;
}

std::string translate(const char* english, int language)
{
    if (language >= 1 && language <= 7)
    {
        for (const Entry& entry : Table)
        {
            if (strcmp(entry.english, english) == 0 && *entry.text[language - 1])
                return latin1(entry.text[language - 1]);
        }
    }
    return english;
}

std::string translate(const char* english)
{
    return translate(english, s_language);
}

}}
