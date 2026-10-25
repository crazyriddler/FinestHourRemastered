#pragma once
// the settings overlay's text in the game's language (overlay_text.cpp)
#include <cstddef>
#include <string>

namespace cod::overlay {

const char* Tr(const char* en);
// the Portuguese (overlay_text_pt.cpp)
struct PtRow {
    const char *en, *pt;
};
extern const PtRow kPtRows[];
extern const size_t kPtRowCount;
// the Russian (overlay_text_ru.cpp)
extern const PtRow kRuRows[];
extern const size_t kRuRowCount;
std::string Tr(const std::string& en);

}  // namespace cod::overlay
