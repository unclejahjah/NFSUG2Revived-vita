#ifndef PLATFORM_TEXTFILE_H_
#define PLATFORM_TEXTFILE_H_

#include <string>

namespace pf
{

// A settings file is unreadable when it exists but cannot be read, is not UTF-8 text, or has no line that parses
// (a comment, a [section] or key=value); one bad line among good ones is not.
enum class TextFile { Missing, Readable, Unreadable };
TextFile readTextFile(const char* path, std::string* text);

}

#endif
