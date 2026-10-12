#pragma once
#include "item_description.hpp"
namespace itemization {
struct CraftItemPresentation {
    std::string rarity="-",item_class="-",quality="-",details="Empty equipment slot.";
};
inline CraftItemPresentation crafting_generated_presentation(const ItemService& service,const Json& item) {
    CraftItemPresentation view;
    view.rarity=item.at("rarity");
    if(!view.rarity.empty())view.rarity[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(view.rarity[0])));
    view.item_class=std::to_string(service.item_class(item));
    view.quality=display_number(quality_percent(item))+"%";
    view.details.clear();
    std::istringstream input(item_description(service,item));std::string line;
    while(std::getline(input,line)) {
        if(line.rfind("Rarity:",0)==0||line.rfind("Quality:",0)==0)continue;
        if(!view.details.empty())view.details+='\n';
        view.details+=line;
    }
    if(view.details.empty())view.details="No implicit stats or modifiers.";
    return view;
}
inline CraftItemPresentation crafting_native_presentation(const Json& base) {
    CraftItemPresentation view;view.rarity="Unmodified";
    if(base.contains("item_class"))view.item_class=std::to_string(base.at("item_class").get<int>());
    // A native +1/+2/+3 variant is not mod quality. Read its actual base stats,
    // without generating an item, rolling modifiers, or changing item history.
    view.details.clear();
    const std::array<std::pair<const char*,const char*>,17> labels{{
        {"attack","Attack"},{"defence","Defence"},{"strength","Strength"},{"resilience","Resilience"},
        {"max_hp","Max HP"},{"max_mp","Max MP"},{"agility","Agility"},{"deftness","Deftness"},
        {"charm","Charm"},{"magical_might","Magical Might"},{"magical_mending","Magical Mending"},
        {"mp_absorption","MP absorption"},{"shield_guard_chance","Shield block chance"},
        {"weapon_guard_chance","Weapon block chance"},{"dodge_chance","Dodge chance"},
        {"critical_chance","Critical chance"},{"zone_chance","Pep chance"}}};
    const auto append=[&](const std::string& text){if(!view.details.empty())view.details+='\n';view.details+=text;};
    const auto& stats=base.at("base_stats");
    for(const auto& label:labels)if(stats.contains(label.first)&&stats.at(label.first).get<double>()!=0)
        append(std::string("Implicit: ")+label.second+" +"+display_number(stats.at(label.first))+(percentage_stat(label.first)?"%":""));
    for(const auto& effect:base.at("implicits"))
        append("Implicit: "+display_stat_label(effect.at("label"))+" +"+display_number(effect.at("value"))+(effect.at("unit")=="percentage_points"?"%":""));
    if(view.details.empty())view.details="No implicit stats or modifiers.";
    return view;
}
}
