#pragma once
#include "item_lifecycle.hpp"
#include "item_display.hpp"
#include <cctype>
#include <iomanip>
#include <sstream>

namespace itemization {
inline std::string display_number(const Json& value) {
    std::ostringstream text;
    text << std::setprecision(12) << value.get<double>();
    return text.str();
}
inline std::string item_description(const ItemService& service,const Json& item) {
    validate_instance(item);
    std::string rarity=item.at("rarity");rarity[0]=static_cast<char>(std::toupper(rarity[0]));
    std::string text="Rarity: "+rarity;
    const int item_class=service.item_class(item);
    if(item_class>0) text+=" | Class: "+std::to_string(item_class);
    text+="\nQuality: "+display_number(quality_percent(item))+"%";
    if(service.is_locked(item))text+="\nLocked against salvage";
    const auto& base=item.at("base_snapshot").at("base_stats");
    const std::array<std::pair<const char*,const char*>,17> labels{{
        {"attack","Attack"},{"defence","Defence"},{"strength","Strength"},{"resilience","Resilience"},
        {"max_hp","Maximum HP"},{"max_mp","Maximum MP"},{"agility","Agility"},{"deftness","Deftness"},
        {"charm","Charm"},{"magical_might","Magical might"},{"magical_mending","Magical mending"},
        {"mp_absorption","MP absorption"},{"shield_guard_chance","Shield block chance"},
        {"weapon_guard_chance","Weapon block chance"},{"dodge_chance","Dodge chance"},
        {"critical_chance","Critical chance"},{"zone_chance","Pep chance"}}};
    // Base stat contributions are the item's innate properties. Display them
    // with the other implicits without moving or duplicating stored effects.
    for(const auto& entry:labels) if(base.contains(entry.first) && base.at(entry.first).get<double>()!=0)
        text+="\nImplicit: "+display_stat_label(entry.second)+" +"+display_number(quality_value(item,base.at(entry.first).get<double>(),entry.first))+
            (percentage_stat(entry.first)?"%":"");
    for(const auto& effect:item.at("implicits"))
        text+="\nImplicit: "+display_stat_label(effect.at("label"))+" +"+display_number(effect_value(item,effect))+
            (effect.at("unit")=="percentage_points"?"%":"");
    for(const std::string kind:{"prefix","suffix"}) for(const auto& effect:item.at("affixes")) {
        if(effect.at("kind")!=kind) continue;
        const auto tier=service.affix_tier(effect);
        const std::string unit=effect.at("unit")=="percentage_points"?"%":"";
        text+="\n"+std::string(kind=="prefix"?"Prefix: ":"Suffix: ")+display_stat_label(effect.at("label"))+
            " +"+display_number(effect_value(item,effect))+unit+" [T"+display_number(tier.at("rank"))+": "+
            display_number(quality_value(item,tier.at("minimum").get<double>(),effect.at("stat")))+"-"+display_number(quality_value(item,tier.at("maximum").get<double>(),effect.at("stat")))+unit+"]";
    }
    return text;
}
}
