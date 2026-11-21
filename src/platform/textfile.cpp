// Whether a settings file is readable text, for the cfg and the mods' ini files alike.

#include <platform/textfile.h>
#include <cstdio>
#include <sys/stat.h>

namespace pf
{

namespace
{

bool validUtf8(const std::string& text, size_t start)
{
    for (size_t i = start; i < text.size();)
    {
        const unsigned char c = (unsigned char)text[i];
        if (c == 0)
            return false;
        const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= text.size() + (extra ? 0 : 1))
            return false;
        for (int k = 1; k <= extra; ++k)
        {
            if (((unsigned char)text[i + k] >> 6) != 2)
                return false;
        }
        i += size_t(extra) + 1;
    }
    return true;
}

bool lineParses(const std::string& text, size_t b, size_t end)
{
    while (b < end && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r'))
        ++b;
    if (b >= end)
        return false;
    if (text[b] == '#' || text[b] == ';' || text.compare(b, 2, "//") == 0)
        return true;
    if (text[b] == '[')
        return text.find(']', b) < end;
    const size_t equals = text.find('=', b);
    return equals < end && equals > b;
}

}

TextFile readTextFile(const char* path, std::string* text)
{
    struct stat info;
    if (stat(path, &info) != 0)
        return TextFile::Missing;
    FILE* file = S_ISREG(info.st_mode) ? fopen(path, "rb") : nullptr;
    if (!file)
        return TextFile::Unreadable;
    text->clear();
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        text->append(buffer, got);
    fclose(file);
    const size_t start = text->compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    if (!validUtf8(*text, start))
        return TextFile::Unreadable;
    for (size_t pos = start; pos < text->size();)
    {
        size_t end = text->find('\n', pos);
        if (end == std::string::npos)
            end = text->size();
        if (lineParses(*text, pos, end))
            return TextFile::Readable;
        pos = end + 1;
    }
    return TextFile::Unreadable;
}

}
