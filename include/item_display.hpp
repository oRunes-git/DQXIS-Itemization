#pragma once
#include <string>
namespace itemization {
// Presentation aliases only: saved labels and stat identities remain intact.
inline std::string display_stat_label(const std::string& label) {
    if(label=="Maximum HP") return "Max HP";
    if(label=="Maximum MP") return "Max MP";
    return label;
}
}
