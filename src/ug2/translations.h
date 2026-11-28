#ifndef UG2_TRANSLATIONS_H_
#define UG2_TRANSLATIONS_H_

#include <string>

// Texts the port added that the game's language files lack, in the game's languages by their English wording.

namespace ug2 { namespace translations {

// The game's language index the texts are chosen for; 0 is English.
void setLanguage(int language);
int language();
// The English text in the language, in the game's text encoding; the English text itself when there is none.
std::string translate(const char* english, int language);
std::string translate(const char* english);

// The table: English wording, then French, German, Italian, Spanish, Dutch, Swedish and Danish, in UTF-8.
struct Entry
{
    const char* english;
    const char* text[7];
};
extern const Entry Table[];
extern const int TableCount;

}}

#endif /* !UG2_TRANSLATIONS_H_ */
