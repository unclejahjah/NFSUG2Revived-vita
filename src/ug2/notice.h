#ifndef UG2_NOTICE_H_
#define UG2_NOTICE_H_

// The start-up notice about settings files that are present but unreadable: 2 s before the renderer starts, only on
// a failure.

#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace notice {

std::string cfgUnreadable(const std::string& cfgPath);
// One line per problem: the cfg (when cfgRead is false), then modLines; empty when all is well.
std::vector<std::string> collect(bool cfgRead, const std::string& cfgPath, std::vector<std::string> modLines);

const uint64_t ShowUs = 2000000;
// The screen, lines stacked upward from the bottom-left (the first at the bottom), each wrapped to fit.
void draw(uint32_t* pixels, int width, int height, int stride, const std::vector<std::string>& lines);
// The drawn rows of text, top to bottom.
std::vector<std::string> rows(const std::vector<std::string>& lines, int width);
// Vita: ShowUs on screen, then every byte of it freed. Host: nothing.
void show(const std::vector<std::string>& lines);

}}

#endif
