#pragma once
#include <set>
#include <string>

namespace itemization {
inline bool direct_damage_stat(const std::string& stat) {
    static const std::set<std::string> stats{"attack","attack_percent","strength","magical_might",
        "critical_damage","spell_damage","evasion_momentum"};
    return stats.count(stat)!=0 || stat.rfind("added_",0)==0 || stat.rfind("damage_",0)==0 ||
        stat.rfind("conditional_damage_",0)==0 || stat.rfind("skill_damage_",0)==0;
}
inline bool native_stat(const std::string& stat) {
    static const std::set<std::string> stats{"attack","defence","agility","deftness","charm","max_hp","max_mp",
        "strength","resilience","magical_might","magical_mending","critical_chance","shield_guard_chance",
        "weapon_guard_chance","dodge_chance","mp_absorption","zone_chance"};
    return stats.count(stat)!=0;
}
inline bool custom_stat(const std::string& stat) {
    static const std::set<std::string> stats{"attack_percent","defence_percent","critical_damage","spell_damage",
        "healing_power","conditional_damage_full_hp","conditional_damage_low_hp","conditional_damage_poisoned",
        "conditional_damage_debuffed","on_hit_poison","on_hit_sleep","on_hit_defence_down","hp_turn","mp_turn",
        "hp_defeat","mp_defeat","life_steal","healing_received","mp_cost","item_healing","physical_reduction",
        "spell_reduction","hp_on_block","evasion_momentum","mp_on_crit","mp_absorption_wand","mp_absorption_heavy_wand"};
    if(stats.count(stat)) return true;
    for(const std::string element:{"fire","ice","wind","earth","lightning","light","dark"})
        if(stat=="damage_"+element || stat=="added_"+element || stat=="added_spell_"+element || stat=="resistance_"+element) return true;
    for(const std::string family:{"dragon","undead","beast","bird","machine","slime","demon","material","elemental"})
        if(stat=="damage_family_"+family) return true;
    for(const std::string status:{"poison","sleep","confusion","paralysis","silence","instant_death"})
        if(stat=="status_resistance_"+status) return true;
    for(const std::string type:{"sword","greatsword","dagger","axe","spear","claw","whip","boomerang"})
        if(stat=="skill_damage_"+type) return true;
    return false;
}
inline bool custom_percentage_stat(const std::string& stat) {
    if(!custom_stat(stat) || stat.rfind("added_",0)==0) return false;
    static const std::set<std::string> flat{"hp_turn","mp_turn","hp_defeat","mp_defeat","hp_on_block","mp_on_crit"};
    return !flat.count(stat);
}
} // namespace itemization
