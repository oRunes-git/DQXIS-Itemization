#pragma once
#include "item_description.hpp"
#include <array>
#include <cmath>

namespace itemization {
enum class CraftAffixChange {None,Added,Changed};
struct CraftDetailRow {
    std::string text,affix_id;
    CraftAffixChange change=CraftAffixChange::None;
};
struct CraftChanges {
    std::string instance_id,result_id;
    std::map<std::string,CraftAffixChange> current;
    bool empty() const {return current.empty();}
};
// Associate the existing description's lines with stable affix families rather
// than list positions, display names, slot IDs or recycled native item IDs.
inline std::vector<CraftDetailRow> craft_detail_rows(const ItemService& service,const Json& item) {
    std::vector<CraftDetailRow> rows;std::array<std::vector<std::string>,2> ids;
    for(const auto& a:item.at("affixes"))ids[a.at("kind")=="prefix"?0:1].push_back(a.at("id"));
    std::array<size_t,2> next{};std::istringstream input(item_description(service,item));std::string line;
    while(std::getline(input,line)) {
        if(line.rfind("Rarity:",0)==0||line.rfind("Quality:",0)==0)continue;
        CraftDetailRow row{line,{}};
        const int kind=line.rfind("Prefix:",0)==0?0:line.rfind("Suffix:",0)==0?1:-1;
        if(kind>=0&&next[kind]<ids[kind].size())row.affix_id=ids[kind][next[kind]++];
        rows.push_back(std::move(row));
    }
    if(rows.empty())rows.push_back({"No implicit stats or modifiers.",{}});
    return rows;
}
inline Json craft_visible_affix(const ItemService& service,const Json& item,const Json& affix) {
    const auto tier=service.affix_tier(affix);
    return {{"kind",affix.at("kind")},{"label",affix.at("label")},{"stat",affix.at("stat")},
        {"unit",affix.at("unit")},{"value",effect_value(item,affix)},{"rank",tier.at("rank")},
        {"minimum",quality_value(item,tier.at("minimum").get<double>(),affix.at("stat"))},
        {"maximum",quality_value(item,tier.at("maximum").get<double>(),affix.at("stat"))}};
}
inline CraftChanges craft_changes(const ItemService& service,const Json& before,const Json& after) {
    require(before.at("instance_id")==after.at("instance_id"),"Craft feedback cannot compare different owned items");
    CraftChanges changes;changes.instance_id=after.at("instance_id");changes.result_id=after.at("engine_id");
    std::map<std::string,const Json*> old;
    for(const auto& a:before.at("affixes"))old.emplace(a.at("id"),&a);
    for(const auto& a:after.at("affixes")) {
        const std::string id=a.at("id");const auto previous=old.find(id);
        if(previous==old.end())changes.current[id]=CraftAffixChange::Added;
        else if(craft_visible_affix(service,before,*previous->second)!=craft_visible_affix(service,after,a))
            changes.current[id]=CraftAffixChange::Changed;
    }
    return changes;
}
inline std::vector<CraftDetailRow> craft_changed_details(const ItemService& service,const Json& item,const CraftChanges* changes) {
    auto rows=craft_detail_rows(service,item);
    // An older revision or another item must never inherit this revision's UI.
    if(!changes||changes->instance_id!=item.at("instance_id")||changes->result_id!=item.at("engine_id"))return rows;
    for(auto& row:rows)if(const auto changed=changes->current.find(row.affix_id);changed!=changes->current.end())row.change=changed->second;
    return rows;
}
inline std::vector<CraftDetailRow> craft_wrap_details(const std::vector<CraftDetailRow>& rows) {
    std::vector<CraftDetailRow> lines;
    for(const auto& row:rows) {
        std::string remaining=row.text;const size_t width=76;
        while(remaining.size()>width) {
            auto at=remaining.rfind(' ',width);if(at==std::string::npos||at<width/3)at=width;
            lines.push_back({remaining.substr(0,at),row.affix_id,row.change});
            remaining.erase(0,at+(remaining[at]==' '?1:0));
        }
        lines.push_back({remaining,row.affix_id,row.change});
    }
    return lines;
}
inline size_t craft_first_change_page(const std::vector<CraftDetailRow>& lines) {
    for(size_t i=0;i<lines.size();++i)if(lines[i].change!=CraftAffixChange::None)return i/8;
    return 0;
}
// A finite two-pulse cue ends at a steady highlight; no worker, game mutation
// or additional native animation binding is needed.
inline float craft_change_pulse(double elapsed_ms) {
    if(elapsed_ms<0||elapsed_ms>=900)return 0;
    const double wave=0.5+0.5*std::cos(elapsed_ms*6.283185307179586/450);
    return static_cast<float>(wave*(1-elapsed_ms/900));
}
}
