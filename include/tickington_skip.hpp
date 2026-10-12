#pragma once
#include <stdexcept>

namespace itemization {
// One native system flag disables the first-visit checkpoint blocker. Do not
// advance Y010, grant pastwords, or synthesize a Tickington return event.
inline constexpr int tickington_system_category=4;
inline constexpr int tickington_scenario_category=5;
inline constexpr const char* tickington_first_return="/Yotti/IsDone_FirstReturnFromYottiMura";
inline constexpr const char* tickington_hotto_complete=u8"/A060/\u226a100\u30b7\u30ca\u30ea\u30aa\u30af\u30ea\u30a2\u5f8c\u226b";

template<class Get,class Set>
bool apply_tickington_skip(Get get,Set set) {
    if(get(tickington_system_category,tickington_first_return)) return false;
    if(!get(tickington_scenario_category,tickington_hotto_complete)) return false;
    set(tickington_system_category,tickington_first_return,true);
    if(!get(tickington_system_category,tickington_first_return))
        throw std::runtime_error("Tickington introduction skip flag write refused");
    return true;
}
}
