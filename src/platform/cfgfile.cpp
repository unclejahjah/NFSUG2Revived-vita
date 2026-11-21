// Settings file text: lookup and in-place changes that keep comments, order and unknown lines.

#include <platform/cfgfile.h>
#include <platform/settings.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

namespace pf
{
namespace cfgfile
{

namespace
{

struct Line
{
    std::string body;       // without the line end
    std::string ending;     // "\n", "\r\n" or empty for a last line without one
};

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trimmed(const std::string& text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && isSpace(text[begin]))
        ++begin;
    while (end > begin && isSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::vector<Line> split(const std::string& text)
{
    std::vector<Line> lines;
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t newline = text.find('\n', pos);
        Line line;
        if (newline == std::string::npos)
        {
            line.body = text.substr(pos);
            pos = text.size();
        }
        else
        {
            line.body = text.substr(pos, newline - pos);
            line.ending = "\n";
            pos = newline + 1;
            if (!line.body.empty() && line.body.back() == '\r')
            {
                line.body.pop_back();
                line.ending = "\r\n";
            }
        }
        lines.push_back(line);
    }
    return lines;
}

// Key of an active line, or empty; skips a BOM.
std::string activeKey(const std::string& body, size_t* valueStart)
{
    size_t start = 0;
    if (body.compare(0, 3, "\xef\xbb\xbf") == 0)
        start = 3;
    while (start < body.size() && isSpace(body[start]))
        ++start;
    if (start >= body.size() || body[start] == '#')
        return std::string();
    const size_t equals = body.find('=', start);
    if (equals == std::string::npos)
        return std::string();
    if (valueStart)
        *valueStart = equals + 1;
    return trimmed(body.substr(start, equals - start));
}

// Key of a commented default such as "#frame_rate=30", or empty.
std::string commentedKey(const std::string& body)
{
    if (body.empty() || body[0] != '#')
        return std::string();
    const size_t equals = body.find('=');
    if (equals == std::string::npos || equals == 1)
        return std::string();
    const std::string key = body.substr(1, equals - 1);
    for (char c : key)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return std::string();
    }
    return key;
}

bool names(const std::string& found, const std::string& key, const char* alias)
{
    return found == key || (alias && found == alias);
}

int findActive(const std::vector<Line>& lines, const std::string& key, const char* alias)
{
    int aliasLine = -1;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string found = activeKey(lines[i].body, nullptr);
        if (found == key)
            return int(i);
        if (alias && aliasLine < 0 && found == alias)
            aliasLine = int(i);
    }
    return aliasLine;
}

}

bool parseBool(const char* text, bool* value)
{
    static const char* const On[] = { "on", "true", "yes", "1" };
    static const char* const Off[] = { "off", "false", "no", "0" };
    for (int i = 0; i < 4; ++i)
    {
        if (strcasecmp(text, On[i]) == 0 || strcasecmp(text, Off[i]) == 0)
        {
            *value = strcasecmp(text, On[i]) == 0;
            return true;
        }
    }
    return false;
}

bool parseInt(const char* text, int* value)
{
    char* end = nullptr;
    errno = 0;
    const long number = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
        return false;
    *value = int(number);
    return true;
}

bool find(const std::string& text, const char* key, std::string* value)
{
    const std::vector<Line> lines = split(text);
    const int at = findActive(lines, key, oldKeyOf(key));
    if (at < 0)
        return false;
    size_t valueStart = 0;
    activeKey(lines[at].body, &valueStart);
    *value = trimmed(lines[at].body.substr(valueStart));
    return true;
}

std::string apply(const std::string& text, const std::vector<Change>& changes)
{
    std::vector<Line> lines = split(text);
    size_t crlf = 0;
    for (const Line& line : lines)
        crlf += line.ending == "\r\n";
    const std::string eol = crlf * 2 > lines.size() ? "\r\n" : "\n";
    for (const Change& change : changes)
    {
        const char* alias = oldKeyOf(change.key.c_str());
        const int at = findActive(lines, change.key, alias);
        if (at >= 0)
        {
            std::string& body = lines[at].body;
            size_t valueStart = 0;
            const std::string key = activeKey(body, &valueStart);
            const size_t bom = body.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
            if (change.atDefault && body.compare(bom, std::string::npos, key + "=" + body.substr(valueStart)) == 0)
            {
                body = body.substr(0, bom) + "#" + key + "=" + change.value;
                continue;
            }
            while (valueStart < body.size() && isSpace(body[valueStart]))
                ++valueStart;
            size_t valueEnd = body.size();
            while (valueEnd > valueStart && isSpace(body[valueEnd - 1]))
                --valueEnd;
            body = body.substr(0, valueStart) + change.value + body.substr(valueEnd);
            continue;
        }
        if (change.atDefault)
            continue;
        const std::string active = change.key + "=" + change.value;
        int comment = -1;
        for (size_t i = 0; i < lines.size() && comment < 0; ++i)
        {
            if (names(commentedKey(lines[i].body), change.key, alias))
                comment = int(i);
        }
        if (comment >= 0)
        {
            lines[comment].body = active;
            continue;
        }
        if (!lines.empty() && lines.back().ending.empty())
            lines.back().ending = eol;
        Line added;
        added.body = active;
        added.ending = eol;
        lines.push_back(added);
    }
    std::string result;
    for (const Line& line : lines)
        result += line.body + line.ending;
    return result;
}

bool readFile(const char* path, std::string* text)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    text->clear();
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        text->append(buffer, got);
    fclose(file);
    return true;
}

bool writeFile(const char* path, const std::string& text)
{
    const std::string temporary = std::string(path) + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
    if (fclose(file) != 0 || !written)
    {
        remove(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path) == 0)
        return true;
    // The Vita refuses to rename over an existing file.
    struct stat status;
    return stat(path, &status) == 0 && remove(path) == 0 && rename(temporary.c_str(), path) == 0;
}

}
}
