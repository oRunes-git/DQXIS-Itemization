#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include "affix_effects.hpp"

namespace itemization {
using Json = nlohmann::json;

struct AffixTier {
    int rank=1,minimum=0,maximum=0,minimum_level=1,minimum_class=1;
    uint32_t generation_weight=100;
    std::string id="t1",label="Tier 1";
    // A resolved tier stores its effective bounds. The unscaled catalogue
    // bounds are retained for provenance, never multiplied again on load.
    double flat_effectiveness=1;
    int base_minimum=0,base_maximum=0;
};
struct Affix {
    std::string id, kind, group, stat, label, unit, name_text;
    std::vector<std::string> categories;
    int minimum = 0, maximum = 0, minimum_level = 1;
    uint32_t weight = 1;
    std::vector<AffixTier> tiers;
    Json policy=Json::object();
    std::string forge_category="Other";
    bool enabled=true;
};
struct Rule { int total, maximum_prefixes, maximum_suffixes; };
struct Request {
    std::string instance_id, base_id, category, rarity;
    int item_level = 1;
    uint64_t seed = 0;
    Json context=Json::object();
};

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument(message);
}

inline bool percentage_stat(const std::string& stat) {
    static const std::set<std::string> stats{"critical_chance","shield_guard_chance","weapon_guard_chance",
        "dodge_chance","mp_absorption","zone_chance"};
    return stats.count(stat)!=0 || custom_percentage_stat(stat);
}
inline bool supported_stat(const std::string& stat) {
    static const std::set<std::string> stats{"attack","defence","agility","deftness","charm","max_hp","max_mp",
        "strength","resilience","magical_might","magical_mending","critical_chance","shield_guard_chance",
        "weapon_guard_chance","dodge_chance","mp_absorption","zone_chance"};
    return stats.count(stat)!=0 || custom_stat(stat);
}

class Random {
    std::mt19937_64 engine;
public:
    explicit Random(uint64_t seed) : engine(seed) {}
    uint64_t below(uint64_t limit) {
        require(limit != 0, "Empty random range");
        // Explicit rejection sampling keeps seeded generation portable across STL implementations.
        const uint64_t threshold = (uint64_t(0) - limit) % limit;
        uint64_t value;
        do { value = engine(); } while (value < threshold);
        return value % limit;
    }
};
inline Json tier_snapshot(const AffixTier& tier) {
    Json result={{"schema_version",1},{"id",tier.id},{"rank",tier.rank},{"label",tier.label},
        {"minimum",tier.minimum},{"maximum",tier.maximum},{"minimum_level",tier.minimum_level}};
    if(tier.flat_effectiveness!=1) {
        result["flat_effectiveness"]=tier.flat_effectiveness;
        result["base_minimum"]=tier.base_minimum;result["base_maximum"]=tier.base_maximum;
    }
    return result;
}
inline void validate_tier(const Json& tier) {
    require(tier.at("schema_version")==1 && !tier.at("id").get<std::string>().empty() &&
        !tier.at("label").get<std::string>().empty(),"Invalid saved affix tier");
    for (const auto field:{"rank","minimum","maximum","minimum_level"}) require(tier.at(field).is_number_integer(),"Tier bounds must be integers");
    const int rank=tier.at("rank"),minimum=tier.at("minimum"),maximum=tier.at("maximum"),level=tier.at("minimum_level");
    require(rank>0 && rank<=100 && level>=1 && minimum>=0 && maximum>=minimum && maximum<=100000,"Invalid tier bounds");
    if(tier.contains("flat_effectiveness")) {
        const double multiplier=tier.at("flat_effectiveness").get<double>();
        require(std::isfinite(multiplier) && multiplier>=1 && multiplier<=4,"Invalid saved flat effectiveness");
        require(tier.at("base_minimum").is_number_integer() && tier.at("base_maximum").is_number_integer(),"Invalid unscaled tier bounds");
        const int lo=tier.at("base_minimum"),hi=tier.at("base_maximum");
        require(lo>=0 && hi>=lo && hi<=100000 && minimum==std::lround(lo*multiplier) && maximum==std::lround(hi*multiplier),
            "Saved tier effectiveness does not match effective bounds");
    }
}
inline Json resolved_affix(const Affix& affix,const AffixTier& tier,int value,int slot) {
    require(value>=tier.minimum && value<=tier.maximum,"Affix value exceeds tier");
    Json result={{"id",affix.id},{"kind",affix.kind},{"group",affix.group},{"stat",affix.stat},
        {"label",affix.label},{"name_text",affix.name_text},{"unit",affix.unit},{"value",value},
        {"slot_id",slot},{"tier",tier_snapshot(tier)}};
    if(!affix.policy.empty()) result["effect_policy"]=affix.policy;
    return result;
}
inline int quality_roll(Random& random,int minimum,int maximum,int draws) {
    require(minimum>=0 && maximum>=minimum && maximum<=100000 && draws>=1 && draws<=16,"Invalid bounded quality roll");
    int value=minimum;
    for (int i=0;i<draws;++i) value=std::max(value,minimum+static_cast<int>(random.below(uint64_t(maximum-minimum)+1)));
    return value;
}

inline constexpr int normal_maximum_item_class=12;
inline constexpr int maximum_item_class=15;
inline constexpr int normal_maximum_affix_tier=24;
inline constexpr int maximum_affix_tier=33;
inline int class_tier_cap(int item_class) {
    require(item_class>=1 && item_class<=maximum_item_class,"Item Class must be 1-15");
    return item_class<=normal_maximum_item_class?item_class*2:
        normal_maximum_affix_tier+(item_class-normal_maximum_item_class)*3;
}
inline bool eligible_affix(const Affix& affix,int level,const std::string& category,const Json& context) {
    if(!affix.enabled || affix.minimum_level>level ||
       std::find(affix.categories.begin(),affix.categories.end(),category)==affix.categories.end()) return false;
    if(context.contains("equipment_type") && affix.policy.contains("eligible_types")) {
        const auto& types=affix.policy.at("eligible_types");
        if(std::find(types.begin(),types.end(),context.at("equipment_type"))==types.end()) return false;
    }
    const auto capability=affix.policy.value("capability",std::string{});
    if(!capability.empty() && context.contains("capabilities")) {
        const auto& available=context.at("capabilities");
        if(std::find(available.begin(),available.end(),capability)==available.end()) return false;
    }
    return true;
}
inline bool eligible_tier(const Affix& affix,const AffixTier& tier,int level,int maximum_rank,const Json& context) {
    if(tier.minimum_level>level || tier.rank>maximum_rank) return false;
    if((affix.stat=="attack_percent" || affix.stat=="defence_percent") && context.contains("base_stats")) {
        const double value=context.at("base_stats").value(affix.stat=="attack_percent"?"attack":"defence",0.0);
        if(value*tier.maximum<100) return false; // Do not roll a tier whose every value resolves to zero.
    }
    if(!context.contains("item_class")) return true; // Original isolated fixtures.
    const int item_class=context.at("item_class");
    const int cap=context.value("class_tier_cap",class_tier_cap(item_class));
    return tier.minimum_class<=item_class && tier.rank<=cap;
}
inline double flat_affix_effectiveness(const Affix& affix,const Json& context) {
    if(percentage_stat(affix.stat) || !context.contains("equipment_type") || !affix.policy.contains("flat_weapon_effectiveness")) return 1;
    return affix.policy.at("flat_weapon_effectiveness").value(context.at("equipment_type").get<std::string>(),1.0);
}
inline AffixTier contextual_tier(const Affix& affix,const AffixTier& source,const Json& context) {
    // One shared base table, with the explicit two-handed flat-affix bonus.
    // This runs before rolling, so every integer in the displayed range can
    // occur and all forge previews/edits use precisely the same bounds.
    require(source.flat_effectiveness==1,"Cannot scale an already resolved tier");
    auto resolved=source;const double multiplier=flat_affix_effectiveness(affix,context);
    require(std::isfinite(multiplier) && multiplier>=1 && multiplier<=4,"Invalid flat affix effectiveness");
    if(multiplier!=1) {
        resolved.flat_effectiveness=multiplier;resolved.base_minimum=source.minimum;resolved.base_maximum=source.maximum;
        resolved.minimum=static_cast<int>(std::lround(source.minimum*multiplier));
        resolved.maximum=static_cast<int>(std::lround(source.maximum*multiplier));
        validate_tier(tier_snapshot(resolved));
    }
    return resolved;
}

class Catalog {
public:
    std::map<std::string, Rule> rules;
    std::vector<Affix> affixes;
    std::vector<int> class_caps;
    Json context_with_class_cap(Json context) const {
        if(context.contains("item_class") && !class_caps.empty()) context["class_tier_cap"]=class_caps.at(context.at("item_class").get<int>()-1);
        return context;
    }
    static Catalog parse(const Json& data) {
        require(data.at("schema_version").get<int>() == 1, "Unsupported catalog schema");
        Catalog result;
        if(data.contains("class_tier_caps")) {
            result.class_caps=data.at("class_tier_caps").get<std::vector<int>>();
            require(result.class_caps.size()==maximum_item_class,"Expected fifteen Class caps");
            int previous=0;for(const int cap:result.class_caps) {require(cap>=previous && cap>=1 && cap<=maximum_affix_tier,"Invalid Class ceiling");previous=cap;}
        }
        for (auto it = data.at("rarities").begin(); it != data.at("rarities").end(); ++it) {
            const auto& row = it.value();
            Rule r{row.at("total"), row.at("maximum_prefixes"), row.at("maximum_suffixes")};
            require(r.total > 0 && r.total <= 16 && r.maximum_prefixes >= 0 &&
                r.maximum_suffixes >= 0 && r.total <= r.maximum_prefixes + r.maximum_suffixes,
                "Invalid rarity rule: " + it.key());
            result.rules.emplace(it.key(), r);
        }
        const std::map<std::string, Rule> expected{{"normal",{1,1,1}}, {"magic",{3,2,2}}, {"rare",{5,3,3}}};
        require(result.rules.size() == expected.size(), "Catalog must define normal, magic and rare");
        for (const auto& pair : expected) {
            auto found = result.rules.find(pair.first);
            require(found != result.rules.end() && found->second.total == pair.second.total &&
                found->second.maximum_prefixes == pair.second.maximum_prefixes &&
                found->second.maximum_suffixes == pair.second.maximum_suffixes, "Catalog rarity rules differ from agreed 1/3/5 policy");
        }
        std::set<std::string> ids;
        std::map<std::string, std::string> group_kinds;
        const std::set<std::string> categories{"weapon", "shield", "head", "body", "accessory"};
        for (const auto& row : data.at("affixes")) {
            Affix a;
            a.id = row.at("id"); a.kind = row.at("kind"); a.group = row.at("group");
            a.stat = row.at("stat"); a.label = row.at("label"); a.unit = row.at("unit");
            a.name_text=row.value("name_text",a.label);
            a.policy=row.value("policy",Json::object());
            require(a.policy.is_object(),"Invalid affix policy");
            if(!a.policy.contains("forge_category")) {
                const std::map<std::string,std::string> groups{
                    {"Shared attributes","Attributes & Chances"},{"Specialisation","Damage & Healing"},
                    {"Elemental offence","Elements"},{"Added elemental damage","Elements"},
                    {"Enemy specialisation","Enemies & Conditions"},{"Conditional offence","Enemies & Conditions"},
                    {"Ailment application","Ailments"},{"Protection","Resistances & Defence"},
                    {"Recovery and utility","Recovery & Utility"},{"Reactive effects","Recovery & Utility"},
                    {"Weapon signatures","Weapon Skills"}};
                const auto section=row.value("section",std::string("Other"));const auto group=groups.find(section);
                a.forge_category=group==groups.end()?section:group->second;
            } else {
                require(a.policy.at("forge_category").is_string(),"Invalid forge affix category");
                a.forge_category=a.policy.at("forge_category");
            }
            require(!a.forge_category.empty(),"Invalid forge affix category");
            a.enabled=row.value("enabled",true);
            require(a.policy.is_object(),"Invalid affix policy");
            if(a.policy.contains("flat_weapon_effectiveness")) {
                const auto& multipliers=a.policy.at("flat_weapon_effectiveness");
                require(multipliers.is_object() && !percentage_stat(a.stat),"Flat effectiveness cannot alter percentage affixes");
                const std::set<std::string> two_handed{"Greatsword","Spear","Claw","Whip","Heavy wand"};
                for(auto it=multipliers.begin();it!=multipliers.end();++it) {
                    const double multiplier=it.value().get<double>();
                    require(two_handed.count(it.key()) && std::isfinite(multiplier) && multiplier>=1 && multiplier<=4,
                        "Invalid two-handed weapon effectiveness mapping");
                }
            }
            a.categories = row.at("categories").get<std::vector<std::string>>();
            a.minimum = row.at("minimum"); a.maximum = row.at("maximum");
            a.minimum_level = row.value("minimum_level", 1);
            const auto weight = row.at("weight").get<int64_t>();
            require(!a.id.empty() && ids.insert(a.id).second, "Empty or duplicate affix ID");
            require(a.kind == "prefix" || a.kind == "suffix", "Invalid affix kind");
            if(a.policy.contains("direct_damage")) {
                require(a.policy.at("direct_damage")==direct_damage_stat(a.stat),"Incorrect direct damage slot classification");
                require(!direct_damage_stat(a.stat) || a.kind=="prefix","Direct damage affixes must be prefixes");
            }
            require(!a.group.empty() && !a.label.empty(), "Empty affix group or label");
            const int width=a.policy.value("minimum_roll_width",0);
            require(width>=0 && width<=1,"Invalid minimum affix variation policy");
            require(a.minimum >= 0 && a.maximum >= a.minimum && a.maximum <= 100000 && a.minimum_level >= 1,
                "Invalid affix roll range: " + a.id);
            require(weight > 0 && weight <= 1000000, "Invalid affix weight: " + a.id);
            require(supported_stat(a.stat), "Unimplemented stat: " + a.stat);
            require(a.unit == (percentage_stat(a.stat) ? "percentage_points" : "points"), "Invalid stat unit");
            require(!a.categories.empty(), "Affix has no equipment categories");
            for (const auto& c : a.categories) require(categories.count(c) != 0, "Unknown category: " + c);
            // Different tiers in one group must stay on the same side. This prevents greedy
            // selection from consuming a suffix group while selecting a prefix (or vice versa).
            auto found = group_kinds.find(a.group);
            require(found == group_kinds.end() || found->second == a.kind, "Affix group crosses prefix/suffix sides");
            group_kinds[a.group] = a.kind;
            a.weight = static_cast<uint32_t>(weight);
            if (row.contains("tiers")) {
                require(row.at("tiers").is_array() && !row.at("tiers").empty(),"Empty affix tier list");
                std::set<int> ranks;std::set<std::string> tier_ids;
                for (const auto& source:row.at("tiers")) {
                    for (const auto field:{"rank","minimum","maximum"}) require(source.at(field).is_number_integer(),"Catalog tier bounds must be integers");
                    AffixTier tier; tier.id=source.at("id");tier.label=source.at("label");tier.rank=source.at("rank");
                    tier.minimum=source.at("minimum");tier.maximum=source.at("maximum");
                    tier.minimum_level=source.value("minimum_level",1);
                    tier.minimum_class=source.value("minimum_class",1);
                    require(tier.minimum_class>=1 && tier.minimum_class<=maximum_item_class,"Invalid tier Class gate");
                    const int64_t tier_weight=source.value("generation_weight",100);
                    require(tier_weight>=0 && tier_weight<=1000000,"Invalid tier generation weight");
                    tier.generation_weight=static_cast<uint32_t>(tier_weight);validate_tier(tier_snapshot(tier));
                    require(ranks.insert(tier.rank).second && tier_ids.insert(tier.id).second,"Duplicate affix tier");
                    a.tiers.push_back(tier);
                }
                std::sort(a.tiers.begin(),a.tiers.end(),[](const AffixTier& x,const AffixTier& y) {return x.rank<y.rank;});
                if(a.policy.value("universal_tier_ranges",false)) {
                    int previous_maximum=0;
                    for(const auto& tier:a.tiers) {
                        require(tier.maximum-tier.minimum>=width && tier.minimum>=previous_maximum,"Universal tier range loses variation or reverses strength");
                        previous_maximum=tier.maximum;
                    }
                }
                require(a.tiers.front().minimum==a.minimum && a.tiers.front().maximum==a.maximum,"First tier must match base roll range");
            } else {
                AffixTier tier;tier.minimum=a.minimum;tier.maximum=a.maximum;tier.minimum_level=a.minimum_level;a.tiers.push_back(tier);
            }
            result.affixes.push_back(std::move(a));
        }
        require(!result.rules.empty() && !result.affixes.empty(), "Empty catalog");
        return result;
    }

    Json generate(Request request) const {
        request.context=context_with_class_cap(request.context);
        require(!request.instance_id.empty() && !request.base_id.empty(), "Item identity is required");
        require(request.item_level >= 1, "Item level must be positive");
        auto rule_entry = rules.find(request.rarity);
        require(rule_entry != rules.end(), "Unknown rarity: " + request.rarity);
        const Rule& rule = rule_entry->second;
        std::vector<const Affix*> pool;
        std::set<std::string> prefix_groups, suffix_groups;
        for (const auto& a : affixes) {
            if(!eligible_affix(a,request.item_level,request.category,request.context)) continue;
            if (std::none_of(a.tiers.begin(),a.tiers.end(),[&](const AffixTier& t) {return eligible_tier(a,t,request.item_level,100,request.context) && t.generation_weight>0;})) continue;
            pool.push_back(&a);
            (a.kind == "prefix" ? prefix_groups : suffix_groups).insert(a.group);
        }
        std::vector<int> splits;
        for (int prefixes = 0; prefixes <= rule.total; ++prefixes) {
            int suffixes = rule.total - prefixes;
            if (prefixes <= rule.maximum_prefixes && suffixes <= rule.maximum_suffixes &&
                prefixes <= static_cast<int>(prefix_groups.size()) && suffixes <= static_cast<int>(suffix_groups.size()))
                splits.push_back(prefixes);
        }
        require(!splits.empty(), "Insufficient eligible affix groups for " + request.category + "/" + request.rarity);
        Random random(request.seed);
        int prefixes = splits[random.below(splits.size())];
        std::set<std::string> used;
        Json rolls = Json::array();
        for (int slot = 0; slot < rule.total; ++slot) {
            std::string side = slot < prefixes ? "prefix" : "suffix";
            uint64_t total_weight = 0;
            for (auto a : pool) if (a->kind == side && !used.count(a->group)) total_weight += a->weight;
            uint64_t selection = random.below(total_weight);
            const Affix* chosen = nullptr;
            for (auto a : pool) {
                if (a->kind != side || used.count(a->group)) continue;
                if (selection < a->weight) { chosen = a; break; }
                selection -= a->weight;
            }
            require(chosen != nullptr, "Affix selection failed");
            uint64_t tier_weight=0;
            int cap=1;
            for(const auto& tier:chosen->tiers) if(eligible_tier(*chosen,tier,request.item_level,100,request.context)) cap=std::max(cap,tier.rank);
            const int floor=request.context.contains("item_class")?std::max(1,cap-3):1;
            for (const auto& tier:chosen->tiers) if (tier.rank>=floor && eligible_tier(*chosen,tier,request.item_level,100,request.context)) tier_weight+=tier.generation_weight;
            uint64_t tier_selection=random.below(tier_weight);const AffixTier* tier_chosen=nullptr;
            for (const auto& tier:chosen->tiers) {
                if (tier.rank<floor || !eligible_tier(*chosen,tier,request.item_level,100,request.context) || tier.generation_weight==0) continue;
                if (tier_selection<tier.generation_weight) {tier_chosen=&tier;break;} tier_selection-=tier.generation_weight;
            }
            require(tier_chosen!=nullptr,"Tier selection failed");
            const auto resolved=contextual_tier(*chosen,*tier_chosen,request.context);
            int value=quality_roll(random,resolved.minimum,resolved.maximum,1);
            used.insert(chosen->group);
            rolls.push_back(resolved_affix(*chosen,resolved,value,slot+1));
        }
        // Resolved values and display metadata are saved, not regenerated from a seed on load.
        return {{"schema_version", 1}, {"instance_id", request.instance_id}, {"base_id", request.base_id}, {"next_affix_slot",rule.total+1},
            {"category", request.category}, {"rarity", request.rarity}, {"item_level", request.item_level},
            {"seed", request.seed}, {"implicits", Json::array()}, {"affixes", rolls}};
    }
};

inline void validate_roll(const Json& roll) {
    require(roll.at("schema_version").get<int>() == 1, "Unsupported saved roll schema");
    require(!roll.at("instance_id").get<std::string>().empty() && !roll.at("base_id").get<std::string>().empty(), "Missing saved item identity");
    const std::set<std::string> categories{"weapon", "shield", "head", "body", "accessory"};
    require(categories.count(roll.at("category").get<std::string>()) != 0, "Invalid saved equipment category");
    require(roll.at("item_level").get<int>() >= 1, "Invalid saved item level");
    require(roll.at("seed").is_number_unsigned() || (roll.at("seed").is_number_integer() && roll.at("seed").get<int64_t>() >= 0), "Invalid saved seed");
    const std::string rarity = roll.at("rarity");
    const std::map<std::string, Rule> rules{{"normal", {1,1,1}}, {"magic", {3,2,2}}, {"rare", {5,3,3}}};
    require(rules.count(rarity) != 0, "Unknown saved rarity");
    const auto& r = rules.at(rarity);
    require(roll.at("affixes").is_array() && roll.at("affixes").size() == static_cast<size_t>(r.total), "Saved affix count mismatch");
    require(roll.at("implicits").is_array() && roll.at("implicits").empty(), "Implicit effects are not implemented yet");
    std::set<std::string> ids, groups;
    int prefixes = 0, suffixes = 0;
    for (const auto& a : roll.at("affixes")) {
        std::string id = a.at("id"), group = a.at("group"), side = a.at("kind");
        require(!id.empty() && !group.empty() && ids.insert(id).second && groups.insert(group).second, "Duplicate saved affix/group");
        require(side == "prefix" || side == "suffix", "Invalid saved affix kind");
        if (side == "prefix") ++prefixes; else ++suffixes;
        const std::string stat = a.at("stat"), unit = a.at("unit");
        require(supported_stat(stat), "Unknown saved stat");
        require(unit == (percentage_stat(stat) ? "percentage_points" : "points"), "Invalid saved stat unit");
        require(a.at("value").is_number_integer() && a.at("value").get<int64_t>() >= 0 && a.at("value").get<int64_t>() <= 100000,
            "Invalid saved affix value");
        require(!a.at("label").get<std::string>().empty(), "Missing saved affix label");
        if (a.contains("tier")) {
            validate_tier(a.at("tier"));
            require(a.at("value")>=a.at("tier").at("minimum") && a.at("value")<=a.at("tier").at("maximum"),"Saved roll exceeds tier");
        }
    }
    require(prefixes <= r.maximum_prefixes && suffixes <= r.maximum_suffixes, "Saved prefix/suffix cap exceeded");
}

inline Json detail_model(const Json& roll) {
    validate_roll(roll);
    Json totals = Json::object(), lines = Json::array();
    for (const auto& a : roll.at("affixes")) {
        std::string stat = a.at("stat"), unit = a.at("unit");
        int value = a.at("value");
        totals[stat] = totals.value(stat, 0) + value;
        lines.push_back({{"label", a.at("label")}, {"kind", a.at("kind")}, {"stat", stat},
            {"value", value}, {"unit", unit}, {"formatted_value", "+" + std::to_string(value) + (unit == "percentage_points" ? " percentage points" : "")}});
    }
    return {{"instance_id", roll.at("instance_id")}, {"base_id", roll.at("base_id")},
        {"rarity", roll.at("rarity")}, {"affix_lines", lines}, {"implicit_lines", roll.at("implicits")}, {"bonuses", totals}};
}
} // namespace itemization
