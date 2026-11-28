// Text edits of the registry file (registry.txt, written by src/lib/registry.cpp): string values, other lines stay byte for byte.

#include <platform/regfile.h>
#include <cstdio>
#include <cstdlib>
#include <strings.h>
#include <vector>

namespace pf
{
namespace regfile
{

namespace
{

const int RegSz = 1;

struct Line
{
    std::string text;       // without its line end
    bool        ended;      // had a line feed
};

std::vector<Line> split(const std::string& text)
{
    std::vector<Line> lines;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        lines.push_back({ text.substr(start, end == std::string::npos ? std::string::npos : end - start),
                          end != std::string::npos });
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

std::string join(const std::vector<Line>& lines)
{
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        text += lines[i].text;
        if (lines[i].ended || i + 1 < lines.size())
            text += '\n';
    }
    return text;
}

std::string trimmed(const std::string& line)
{
    return line.substr(0, line.find_last_not_of("\r") + 1);
}

bool isHeader(const std::string& line, const std::string& key)
{
    const std::string t = trimmed(line);
    return t.size() == key.size() + 2 && t[0] == '[' && t.back() == ']' && strcasecmp(t.c_str() + 1, (key + "]").c_str()) == 0;
}

bool valueLine(const std::string& line, const std::string& name)
{
    const size_t tab = line.find('\t');
    return tab == name.size() && strncasecmp(line.c_str(), name.c_str(), tab) == 0;
}

std::string valueText(const std::string& name, const std::string& value)
{
    std::string line = name + "\t" + std::to_string(RegSz) + "\t";
    char hex[3];
    for (size_t i = 0; i <= value.size(); ++i)
    {
        snprintf(hex, sizeof(hex), "%02x", i < value.size() ? unsigned(static_cast<unsigned char>(value[i])) : 0u);
        line += hex;
    }
    return line;
}

// The line range [first, last) of a key's values, or false.
bool section(const std::vector<Line>& lines, const std::string& key, size_t* first, size_t* last)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!isHeader(lines[i].text, key))
            continue;
        *first = i + 1;
        *last = *first;
        while (*last < lines.size() && (lines[*last].text.empty() || lines[*last].text[0] != '['))
            ++*last;
        return true;
    }
    return false;
}

}

bool findString(const std::string& text, const std::string& key, const std::string& name, std::string* value)
{
    const std::vector<Line> lines = split(text);
    size_t first, last;
    if (!section(lines, key, &first, &last))
        return false;
    for (size_t i = first; i < last; ++i)
    {
        if (!valueLine(lines[i].text, name))
            continue;
        const std::string t = trimmed(lines[i].text);
        const size_t typeEnd = t.find('\t', name.size() + 1);
        if (typeEnd == std::string::npos || atoi(t.c_str() + name.size() + 1) != RegSz)
            return false;
        std::string out;
        for (size_t p = typeEnd + 1; p + 1 < t.size(); p += 2)
        {
            const char pair[3] = { t[p], t[p + 1], 0 };
            const unsigned char byte = static_cast<unsigned char>(strtoul(pair, nullptr, 16));
            if (!byte)
                break;
            out += char(byte);
        }
        *value = out;
        return true;
    }
    return false;
}

std::string setString(const std::string& text, const std::string& key, const std::string& name, const std::string& value)
{
    std::vector<Line> lines = split(text);
    const Line line = { valueText(name, value), true };
    size_t first, last;
    if (!section(lines, key, &first, &last))
    {
        if (!lines.empty())
            lines.back().ended = true;
        lines.push_back({ "[" + key + "]", true });
        lines.push_back(line);
        return join(lines);
    }
    for (size_t i = first; i < last; ++i)
    {
        if (valueLine(lines[i].text, name))
        {
            lines[i].text = valueText(lines[i].text.substr(0, name.size()), value);
            return join(lines);
        }
    }
    if (last > first)
        lines[last - 1].ended = true;
    lines.insert(lines.begin() + long(last), line);
    return join(lines);
}

}
}
