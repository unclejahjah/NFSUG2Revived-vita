// The start-up notices' text and which files need one; the launcher shows the same text.

#include "notice.h"

namespace ug2 { namespace notice {

std::string cfgUnreadable(const std::string& cfgPath)
{
    return cfgPath + " is unable to be read, using default settings";
}

std::vector<std::string> collect(bool cfgRead, const std::string& cfgPath, std::vector<std::string> modLines)
{
    std::vector<std::string> lines;
    if (!cfgRead)
        lines.push_back(cfgUnreadable(cfgPath));
    lines.insert(lines.end(), modLines.begin(), modLines.end());
    return lines;
}

}}
