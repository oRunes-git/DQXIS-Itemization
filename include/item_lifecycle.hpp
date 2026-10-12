#pragma once
#include "itemization.hpp"
#include <cmath>
#include <functional>

namespace itemization {
enum class AffixReplacementResult {Failure,Success,Perfect};
// This is the domain model. No bag index, compiled-roll pool or game UI owns an
// item. Every game-facing view derives from one resolved, versioned instance.
inline const std::map<std::string,Rule>& lifetime_rules() {
    static const std::map<std::string,Rule> rules{{"normal",{1,1,1}},{"magic",{3,2,2}},{"rare",{5,3,3}}};
    return rules;
}
inline void validate_effect(const Json& effect, bool affix) {
    std::string stat=effect.at("stat"),unit=effect.at("unit");
    require(supported_stat(stat),"Unsupported effect stat");
    require(unit==(percentage_stat(stat)?"percentage_points":"points"),"Effect unit does not match stat");
    require(!effect.at("id").get<std::string>().empty() && !effect.at("label").get<std::string>().empty(),"Effect identity/label missing");
    require(effect.at("value").is_number(),"Effect value must be numeric");
    double value=effect.at("value").get<double>();
    require(std::isfinite(value) && value>=0 && value<=100000,"Invalid effect value");
    require(percentage_stat(stat) || std::floor(value)==value,"Point stat must be integral");
    if (affix) {
        require(effect.at("kind")=="prefix" || effect.at("kind")=="suffix","Invalid affix side");
        require(!effect.at("group").get<std::string>().empty(),"Affix group missing");
        if (effect.contains("tier")) {
            validate_tier(effect.at("tier"));
            require(value>=effect.at("tier").at("minimum").get<int>() && value<=effect.at("tier").at("maximum").get<int>(),"Affix exceeds saved tier bounds");
        }
        if (effect.contains("slot_id")) require(effect.at("slot_id").get<int>()>0,"Invalid affix slot identity");
    }
}
inline void validate_base(const Json& base) {
    require(!base.at("base_id").get<std::string>().empty() && !base.at("native_base_id").get<std::string>().empty(),"Base identity missing");
    require(base.at("version").get<int>()>=1 && base.at("item_level").get<int>()>=1,"Invalid base version/level");
    const std::set<std::string> categories{"weapon","shield","head","body","accessory"};
    require(categories.count(base.at("category").get<std::string>())!=0,"Invalid base category");
    require(!base.at("display_name").get<std::string>().empty(),"Base name missing");
    if(base.contains("item_class")) require(base.at("item_class").is_number_integer() && base.at("item_class").get<int>()>=1 && base.at("item_class").get<int>()<=12,"Invalid base Class");
    if(base.contains("equipment_type")) require(base.at("equipment_type").is_string() && !base.at("equipment_type").get<std::string>().empty(),"Missing equipment type");
    if(base.contains("capabilities")) require(base.at("capabilities").is_array(),"Invalid wearer capability list");
    require(base.at("native_fields").is_object() && base.at("base_stats").is_object() && base.at("implicits").is_array(),"Invalid base payload");
    for (auto it=base.at("base_stats").begin();it!=base.at("base_stats").end();++it)
        validate_effect({{"id",it.key()},{"label",it.key()},{"stat",it.key()},
            {"unit",percentage_stat(it.key())?"percentage_points":"points"},{"value",it.value()}},false);
    std::set<std::string> ids;
    for (const auto& effect:base.at("implicits")) {
        validate_effect(effect,false);
        require(ids.insert(effect.at("id").get<std::string>()).second,"Duplicate implicit identity");
    }
}
class BaseRegistry {
    std::map<std::string,Json> bases;
public:
    static Json resolve(const Json& data) {
        require(data.at("schema_version")==1 && data.at("bases").is_array(),"Unsupported base catalog");
        std::map<std::string,Json> authored,resolved;std::set<std::string> visiting;
        for (const auto& base:data.at("bases"))
            require(authored.emplace(base.at("base_id").get<std::string>(),base).second,"Duplicate item base");
        std::function<Json(const std::string&)> visit=[&](const std::string& id)->Json {
            if (resolved.count(id)) return resolved.at(id);
            require(authored.count(id)!=0,"Inherited base not found: "+id);
            require(visiting.insert(id).second,"Cyclic base inheritance");
            const auto& definition=authored.at(id);Json base=Json::object();
            if (definition.contains("extends")) base=visit(definition.at("extends"));
            for (auto it=definition.begin();it!=definition.end();++it) {
                if ((it.key()=="base_stats" || it.key()=="native_fields") && base.contains(it.key()))
                    base[it.key()].merge_patch(it.value());
                else base[it.key()]=it.value();
            }
            validate_base(base);visiting.erase(id);resolved.emplace(id,base);return base;
        };
        Json list=Json::array();for (const auto& base:data.at("bases")) list.push_back(visit(base.at("base_id")));
        return {{"schema_version",1},{"bases",list}};
    }
    static BaseRegistry parse(const Json& data) {
        auto complete=resolve(data);
        BaseRegistry result;
        for (const auto& base:complete.at("bases")) {
            validate_base(base);
            require(result.bases.emplace(base.at("base_id").get<std::string>(),base).second,"Duplicate item base");
        }
        return result;
    }
    const Json& get(const std::string& id) const {return bases.at(id);}
    bool contains(const std::string& id) const {return bases.count(id)!=0;}
    size_t size() const {return bases.size();}
};
inline std::string engine_identity(const std::string& logical_id,int revision) {
    require(logical_id.size()==32 && std::all_of(logical_id.begin(),logical_id.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');}),"Logical item identity must be a 128-bit hex ID");
    require(revision>0,"Invalid item revision");
    return "ARPG_I_"+logical_id+"_R_"+std::to_string(revision);
}
inline void validate_instance(const Json& item,bool full=false) {
    require(item.at("schema_version")==2,"Unsupported item instance schema");
    if(item.contains("quality_bp")) require(item.at("quality_bp").is_number_integer() && item.at("quality_bp").get<int>()>=0 && item.at("quality_bp").get<int>()<=2500,"Invalid equipment quality");
    const auto& base=item.at("base_snapshot");validate_base(base);
    if(item.contains("item_class_override")) {
        require(item.at("item_class_override").is_number_integer(),"Invalid upgraded item Class");
        const int upgraded=item.at("item_class_override");
        require(upgraded>=1 && upgraded<=maximum_item_class && upgraded>=base.value("item_class",1),"Invalid upgraded item Class");
    }
    require(item.at("base_id")==base.at("base_id") && item.at("category")==base.at("category"),"Instance/base mismatch");
    require(item.at("item_level").get<int>()>=1,"Invalid instance level");
    require(item.at("engine_id")==engine_identity(item.at("instance_id"),item.at("revision")),"Engine identity does not match logical revision");
    std::string rarity=item.at("rarity");
    require(lifetime_rules().count(rarity)!=0,"Invalid item rarity");
    const auto& rule=lifetime_rules().at(rarity);
    require(item.at("affixes").is_array() && item.at("implicits").is_array(),"Invalid instance effects");
    require(item.at("affixes").size()<=static_cast<size_t>(rule.total),"Affix capacity exceeded");
    if (full) require(item.at("affixes").size()==static_cast<size_t>(rule.total),"Generation must fill rarity capacity");
    int prefixes=0,suffixes=0;std::set<std::string> ids,groups;std::set<int> slots;
    for (const auto& effect:item.at("affixes")) {
        validate_effect(effect,true);
        require(ids.insert(effect.at("id").get<std::string>()).second && groups.insert(effect.at("group").get<std::string>()).second,"Duplicate affix/group");
        if (effect.contains("slot_id")) require(slots.insert(effect.at("slot_id").get<int>()).second,"Duplicate affix slot identity");
        if (effect.at("kind")=="prefix") ++prefixes;else ++suffixes;
    }
    require(prefixes<=rule.maximum_prefixes && suffixes<=rule.maximum_suffixes,"Affix side cap exceeded");
    ids.clear();
    for (const auto& effect:item.at("implicits")) {
        validate_effect(effect,false);
        require(ids.insert(effect.at("id").get<std::string>()).second,"Duplicate instance implicit");
    }
}
// Stored rolls and tier ranges never change with quality. Resolve the multiplier
// once, at display/stat/combat projection. Point bonuses round down;
// percentages round up to whole points. Reforging cannot compound the bonus.
inline int quality_percent(const Json& item) {
    const int bp=item.value("quality_bp",0);require(bp>=0 && bp<=2500,"Invalid equipment quality");
    return (bp+99)/100; // Also project old fractional-quality saves without rewriting history.
}
inline int quality_from_score(double score) {
    require(std::isfinite(score) && score>=0 && score<=100,"Invalid quality score");
    return static_cast<int>(std::ceil(score/4.0))*100;
}
inline double quality_value(const Json& item,double value,const std::string& stat) {
    // Integer quality avoids floating-point products crossing an exact percent boundary.
    const double result=value*(100+quality_percent(item))/100.0;
    return percentage_stat(stat)?std::ceil(result):std::floor(result+1e-9);
}
inline double effect_value(const Json& item,const Json& effect) {
    return quality_value(item,effect.at("value").get<double>(),effect.at("stat"));
}
inline Json effective_stats(const Json& item) {
    validate_instance(item);
    Json totals=item.at("base_snapshot").at("base_stats");
    for(auto s=totals.begin();s!=totals.end();++s) s.value()=quality_value(item,s.value().get<double>(),s.key());
    for (const auto& key:{"implicits","affixes"}) for (const auto& effect:item.at(key)) {
        std::string stat=effect.at("stat");
        if(!native_stat(stat)) continue;
        double value=totals.value(stat,0.0)+effect_value(item,effect);
        require(std::isfinite(value) && value<=100000,"Resulting stat exceeds supported range");
        totals[stat]=value;
    }
    // Percent bonuses use the unmodified base, never previously added stats.
    for(const auto& effect:item.at("affixes")) {
        const std::string stat=effect.at("stat");
        if(stat=="attack_percent" || stat=="defence_percent") {
            const std::string target=stat=="attack_percent"?"attack":"defence";
            const auto& base_stats=item.at("base_snapshot").at("base_stats");
            totals[target]=totals.value(target,0.0)+std::floor(base_stats.value(target,0.0)*effect_value(item,effect)/100.0);
        }
        if(stat=="mp_absorption_wand" || stat=="mp_absorption_heavy_wand") {
            double innate=item.at("base_snapshot").at("base_stats").value("mp_absorption",0.0);
            for(const auto& implicit:item.at("implicits")) if(implicit.at("stat")=="mp_absorption") innate+=implicit.at("value").get<double>();
            totals["mp_absorption"]=totals.value("mp_absorption",0.0)+innate*effect_value(item,effect)/100.0;
        }
    }
    // Derived native percentages (for example MP absorption) can become
    // fractional even when their contributing modifiers are whole percentages.
    for(auto stat=totals.begin();stat!=totals.end();++stat)
        if(percentage_stat(stat.key())) stat.value()=std::ceil(stat.value().get<double>());
    return totals;
}
inline Json instance_details(const Json& item) {
    validate_instance(item);
    const auto& rule=lifetime_rules().at(item.at("rarity").get<std::string>());
    return {{"instance_id",item.at("instance_id")},{"engine_id",item.at("engine_id")},{"revision",item.at("revision")},
        {"base_id",item.at("base_id")},{"base_name",item.at("base_snapshot").at("display_name")},
        {"rarity",item.at("rarity")},{"affix_capacity",rule.total},{"affix_count",item.at("affixes").size()},
        {"affix_lines",item.at("affixes")},{"implicit_lines",item.at("implicits")},{"effective_stats",effective_stats(item)}};
}
inline std::string instance_name(const Json& item) {
    validate_instance(item);
    std::string result=item.at("base_snapshot").at("display_name");
    // Keep menu names manageable; the detail view lists every modifier separately.
    bool prefix=false,suffix=false;
    for (const auto& effect:item.at("affixes")) {
        auto text=effect.value("name_text",effect.at("label").get<std::string>());
        if (effect.at("kind")=="prefix" && !prefix) {result=text+" "+result;prefix=true;}
        if (effect.at("kind")=="suffix" && !suffix) {result+=" "+text;suffix=true;}
    }
    return result;
}
class ItemService {
    BaseRegistry bases;
    Catalog catalog;
    // Append-only revision history permits loading an older adventure log: its
    // engine ID resolves exactly that old revision, never a global latest roll.
    std::map<std::string,Json> revisions;
    std::map<std::string,int> next_revisions;
    // Protection is a durable preference for one logical item, shared by all
    // of its revisions. It never rewrites a roll or changes a native item ID.
    std::map<std::string,bool> equipment_locks;
    int next_revision(const std::string& id) const {
        auto found=next_revisions.find(id);return found==next_revisions.end()?1:found->second;
    }
    Json draft(const std::string& engine_id) const {
        Json item=lookup(engine_id);
        item["parent_engine_id"]=engine_id;
        item["revision"]=next_revision(item.at("instance_id"));
        item["engine_id"]=engine_identity(item.at("instance_id"),item.at("revision"));
        return item;
    }
    static void enhance_values(Json& item,const std::string& affix_id,int amount,int ceiling) {
        require(amount>0 && amount<=100000 && ceiling>0 && ceiling<=100000,"Invalid enhancement bounds");
        auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=effects.end(),"Affix not found for enhancement");
        require(target->at("value").is_number_integer(),"Integer enhancement requires an integer modifier");
        const int64_t current=target->at("value").get<int64_t>();
        if (target->contains("tier")) require(ceiling<=target->at("tier").at("maximum").get<int>(),"Use a tier operation to exceed current tier");
        require(current<ceiling,"Modifier already meets enhancement ceiling");
        (*target)["value"]=std::min<int64_t>(current+amount,ceiling);
        validate_instance(item);effective_stats(item);
    }
    const Affix& definition(const Json& effect) const {
        auto found=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a) {return a.id==effect.at("id");});
        require(found!=catalog.affixes.end(),"Retired affix requires explicit migration for type/tier changes");
        bool slot_identity=found->kind==effect.at("kind") && found->group==effect.at("group");
        for(const auto& legacy:found->policy.value("legacy_slot_identities",Json::array()))
            slot_identity=slot_identity || (legacy.at("kind")==effect.at("kind") && legacy.at("group")==effect.at("group"));
        require(found->stat==effect.at("stat") && found->unit==effect.at("unit") && slot_identity,"Catalog affix conflicts with saved identity");
        return *found;
    }
    Json item_context(const Json& item) const {
        const auto& snapshot=item.at("base_snapshot");
        const auto& source=snapshot.contains("item_class")?snapshot:
            (bases.contains(item.at("base_id"))?bases.get(item.at("base_id")):snapshot);
        Json context={{"base_stats",snapshot.at("base_stats")}};
        for(const auto key:{"equipment_type","item_class","capabilities"})
            if(source.contains(key)) context[key]=source.at(key);
        if(item.contains("item_class_override")) context["item_class"]=item.at("item_class_override");
        return catalog.context_with_class_cap(context);
    }
    Json item_tier(const Affix& affix,const AffixTier& tier,const Json& item) const {
        return tier_snapshot(contextual_tier(affix,tier,item_context(item)));
    }
    std::vector<const AffixTier*> eligible_tiers(const Affix& affix,const Json& item,int maximum_rank) const {
        std::vector<const AffixTier*> result;
        const auto context=item_context(item);const int level=item.at("item_level");
        if(!eligible_affix(affix,level,item.at("category"),context)) return result;
        for(const auto& tier:affix.tiers) if(eligible_tier(affix,tier,level,maximum_rank,context)) result.push_back(&tier);
        return result;
    }
    std::vector<const Affix*> replacements(const Json& item,const Json& target,int maximum_rank) const {
        // Replacement must change family. Reserve the target's group as well as
        // other slots; keeping a family belongs to value/tier reroll operations.
        std::set<std::string> used,used_ids;
        int remaining_prefixes=0,remaining_suffixes=0,remaining_damage=0;
        for (const auto& effect:item.at("affixes")) {
            used.insert(effect.at("group"));
            used_ids.insert(effect.at("id"));
            const auto current=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a){return a.id==effect.at("id");});
            if(current!=catalog.affixes.end()) used.insert(current->group);
            if(effect.at("id")==target.at("id")) continue; // Free the targeted slot before checking sides.
            if(effect.at("kind")=="prefix") ++remaining_prefixes;else ++remaining_suffixes;
            if(direct_damage_stat(effect.at("stat"))) ++remaining_damage;
        }
        const auto& rule=lifetime_rules().at(item.at("rarity").get<std::string>());
        std::vector<const Affix*> result;
        for (const auto& candidate:catalog.affixes) {
            if (used.count(candidate.group) || used_ids.count(candidate.id) ||
                std::find(candidate.categories.begin(),candidate.categories.end(),item.at("category").get<std::string>())==candidate.categories.end()) continue;
            if(remaining_prefixes+(candidate.kind=="prefix")>rule.maximum_prefixes ||
                remaining_suffixes+(candidate.kind=="suffix")>rule.maximum_suffixes) continue;
            if(item.at("category")=="weapon" && remaining_damage+direct_damage_stat(candidate.stat)>rule.maximum_prefixes) continue;
            if (!eligible_tiers(candidate,item,maximum_rank).empty()) result.push_back(&candidate);
        }
        return result;
    }
public:
    ItemService(BaseRegistry base_catalog,Catalog affix_catalog):bases(std::move(base_catalog)),catalog(std::move(affix_catalog)) {}
    Json reforge_quality(const std::string& engine_id,double score) const {
        Json item=draft(engine_id);item["quality_bp"]=quality_from_score(score);
        validate_instance(item);effective_stats(item);return item;
    }
    Json finish_quality(Json generated,double score) const {
        require(generated.at("revision")==1 && generated.at("parent_engine_id").is_null(),"Recipe quality requires a fresh instance");
        generated["quality_bp"]=quality_from_score(score);validate_instance(generated);effective_stats(generated);return generated;
    }
    std::set<std::string> addable_affix_families(const Json& item) const {
        validate_instance(item);
        std::set<std::string> families;
        if(item.at("affixes").size()>=static_cast<size_t>(lifetime_rules().at(item.at("rarity").get<std::string>()).total)) return families;
        // Reserve every existing slot; no target is freed. Reuse the same
        // group, side, damage-count, type and Class eligibility as replacements.
        for(const auto* affix:replacements(item,Json{{"id",""}},100)) families.insert(affix->id);
        return families;
    }
    std::set<std::string> shard_craftable_affix_families(const Json& item) const {
        auto families=addable_affix_families(item);
        for(const auto& effect:item.at("affixes")) {
            const auto source=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a){return a.id==effect.at("id");});
            if(source!=catalog.affixes.end()&&eligible_affix(*source,item.at("item_level"),item.at("category"),item_context(item))&&
                can_craft(item,effect.at("id"),"upgrade_tier",100))families.insert(effect.at("id"));
        }
        return families;
    }
    Json craft_shard(const std::string& engine_id,const std::string& family,uint64_t seed,const std::vector<uint32_t>& weights) const {
        require(weights.size()==maximum_affix_tier,"Expected thirty-three shard tier weights");
        for(auto w:weights) require(w>0 && w<=1000000,"Invalid shard tier weight");
        const auto& current=lookup(engine_id);
        for(const auto& effect:current.at("affixes"))if(effect.at("id")==family) {
            require(eligible_affix(definition(effect),current.at("item_level"),current.at("category"),item_context(current)),"Shard modifier is restricted on this item");
            return craft(engine_id,family,"upgrade_tier",seed,1,100);
        }
        Json item=add_affix(engine_id,family,seed); // Shared type, slot, conflict and damage-cap checks.
        auto& effect=item["affixes"].back();const auto& source=definition(effect);
        const auto tiers=eligible_tiers(source,item,100);uint64_t total=0;
        for(const auto* t:tiers) total+=weights.at(t->rank-1);
        require(total>0,"No eligible shard tier");Random random(seed);auto draw=random.below(total);
        for(const auto* t:tiers) {const auto w=weights.at(t->rank-1);if(draw<w) {
            const auto resolved=contextual_tier(source,*t,item_context(item));
            effect=resolved_affix(source,resolved,quality_roll(random,resolved.minimum,resolved.maximum,1),effect.at("slot_id"));break;
        }draw-=w;}
        validate_instance(item);effective_stats(item);return item;
    }
    // Currency drafts use the same immutable history and legality rules as all
    // other edits. Tier weights belong to crafting, not acquisition generation.
    Json currency_edit(const std::string& engine_id,const std::string& operation,
        const std::string& target_id,uint64_t seed,const std::vector<uint32_t>& tier_weights) const {
        const Json old=lookup(engine_id);validate_instance(old);
        require(tier_weights.size()==maximum_affix_tier,"Expected thirty-three crafting tier weights");
        for(auto w:tier_weights) require(w>0 && w<=1000000,"Invalid crafting tier weight");
        if(operation=="kingmaker") {
            require(target_id.empty(),"Class elevation has no modifier target");
            const int current=item_class(old);require(current>=1 && current<maximum_item_class,"Item is already at the maximum Class");
            Json item=draft(engine_id);item["item_class_override"]=current+1;
            validate_instance(item);effective_stats(item);return item;
        }
        Random random(seed);
        if(operation=="tuning" || operation=="surging") {
            require(can_craft(old,target_id,operation=="tuning"?"reroll_value":"upgrade_tier",100),"Selected modifier is unavailable");
            return craft(engine_id,target_id,operation=="tuning"?"reroll_value":"upgrade_tier",seed,1,100);
        }
        Json item=draft(engine_id);auto& effects=item["affixes"];
        const std::string rarity=old.at("rarity");int additions=0;
        if(operation=="severing") {
            require(!effects.empty(),"No modifier to remove");effects.erase(effects.begin()+random.below(effects.size()));
        } else {
            if(operation=="shifting" || operation=="tempest") {
                require(operation=="shifting"?(rarity=="normal" || rarity=="magic"):rarity=="rare","This Veyl does not support the item's rarity");
                effects=Json::array();additions=lifetime_rules().at(rarity).total;
            } else if(operation=="waking" || operation=="blooming") {
                require(rarity==(operation=="waking"?"normal":"magic"),"This Veyl needs a different rarity");
                item["rarity"]=operation=="waking"?"magic":"rare";additions=1;
            } else if(operation=="binding") {
                require(effects.size()<static_cast<size_t>(lifetime_rules().at(rarity).total),"No empty modifier slot");additions=1;
            } else throw std::invalid_argument("Unknown Veyl operation");
            const auto frozen_context=item_context(item);
            std::vector<const Affix*> base_pool;
            std::map<std::string,std::vector<const AffixTier*>> base_tiers;
            for(const auto& a:catalog.affixes) {
                auto ts=eligible_tiers(a,item,100);if(ts.empty())continue;
                base_pool.push_back(&a);base_tiers.emplace(a.id,std::move(ts));
            }
            auto candidates=[&](const Json& state) {
                std::vector<const Affix*> pool;std::set<std::string> ids,groups;
                int prefixes=0,suffixes=0,damage=0;
                for(const auto& e:state.at("affixes")) {
                    ids.insert(e.at("id"));groups.insert(e.at("group"));
                    const auto d=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a){return a.id==e.at("id");});
                    if(d!=catalog.affixes.end()) groups.insert(d->group);
                    e.at("kind")=="prefix"?++prefixes:++suffixes;
                    if(direct_damage_stat(e.at("stat"))) ++damage;
                }
                const auto& rule=lifetime_rules().at(state.at("rarity").get<std::string>());
                for(const auto* pointer:base_pool) {const auto& a=*pointer;
                    if(ids.count(a.id) || groups.count(a.group) ||
                        prefixes+(a.kind=="prefix")>rule.maximum_prefixes || suffixes+(a.kind=="suffix")>rule.maximum_suffixes ||
                        (state.at("category")=="weapon" && damage+direct_damage_stat(a.stat)>rule.maximum_prefixes)) continue;
                    pool.push_back(&a);
                }
                return pool;
            };
            // Filter choices which would strand the remaining slots. This also
            // handles future conflict groups shared across prefix/suffix sides.
            std::function<bool(const Json&,int)> can_fill=[&](const Json& state,int count) {
                if(count==0) return true;
                for(const auto* a:candidates(state)) {
                    Json next={{"affixes",state.at("affixes")},{"rarity",state.at("rarity")},{"category",state.at("category")}};
                    const auto t=base_tiers.at(a->id).front();
                    const auto resolved=contextual_tier(*a,*t,frozen_context);
                    next["affixes"].push_back(resolved_affix(*a,resolved,resolved.minimum,1));
                    if(can_fill(next,count-1)) return true;
                }
                return false;
            };
            for(int i=0;i<additions;++i) {
                std::vector<const Affix*> pool;
                for(const auto* a:candidates(item)) {
                    Json next={{"affixes",item.at("affixes")},{"rarity",item.at("rarity")},{"category",item.at("category")}};
                    const auto t=base_tiers.at(a->id).front();
                    const auto resolved=contextual_tier(*a,*t,frozen_context);
                    next["affixes"].push_back(resolved_affix(*a,resolved,resolved.minimum,1));
                    if(can_fill(next,additions-i-1)) pool.push_back(a);
                }
                uint64_t total=0;for(const auto* a:pool) total+=a->weight;
                require(total>0,"No eligible modifiers for this Veyl");auto draw=random.below(total);const Affix* chosen=nullptr;
                for(const auto* a:pool) {if(draw<a->weight) {chosen=a;break;}draw-=a->weight;}
                const auto& tiers=base_tiers.at(chosen->id);total=0;
                for(const auto* t:tiers) total+=tier_weights.at(t->rank-1);
                draw=random.below(total);const AffixTier* tier=nullptr;
                for(const auto* t:tiers) {const auto w=tier_weights.at(t->rank-1);if(draw<w) {tier=t;break;}draw-=w;}
                const auto resolved=contextual_tier(*chosen,*tier,frozen_context);
                int slot=item.value("next_affix_slot",1);for(const auto& e:effects) slot=std::max(slot,e.value("slot_id",0)+1);
                item["next_affix_slot"]=slot+1;
                effects.push_back(resolved_affix(*chosen,resolved,quality_roll(random,resolved.minimum,resolved.maximum,1),slot));
            }
        }
        validate_instance(item);effective_stats(item);return item;
    }
    int item_class(const Json& item) const {return item_context(item).value("item_class",0);}
    Json generate(const std::string& base_id,const std::string& rarity,uint64_t seed,const std::string& identity) const {
        require(next_revision(identity)==1,"Item identity is already issued");
        const auto& base=bases.get(base_id);
        Request request{identity,base_id,base.at("category"),rarity,base.at("item_level"),seed};
        request.context={{"base_stats",base.at("base_stats")}};
        for(const auto key:{"equipment_type","item_class","capabilities"}) if(base.contains(key)) request.context[key]=base.at(key);
        Json item=catalog.generate(request); // Fresh roll at the actual acquisition request.
        item["schema_version"]=2;item["base_snapshot"]=base;item["implicits"]=base.at("implicits");
        item["revision"]=1;item["engine_id"]=engine_identity(identity,1);item["parent_engine_id"]=nullptr;
        validate_instance(item,true);effective_stats(item);return item;
    }
    // Explicit development fixture only. Normal acquisition never calls this:
    // it deliberately bypasses type/Class restrictions for guaranteed-crit tests.
    Json generate_critical_test_item(const std::string& base_id,uint64_t seed,const std::string& identity) const {
        require(base_id=="E_CLO_0052" || base_id=="E_HEL_0024" || base_id=="A_ARM_0007","Unsupported critical test base");
        Json item=generate(base_id,"normal",seed,identity);
        const auto& base=item.at("base_snapshot");
        require(base.at("native_fields").at("bEquipmentPossible_P001")=="true","Critical fixture must be wearable by Hero");
        auto affix=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[](const Affix& a){return a.enabled && a.stat=="critical_chance";});
        require(affix!=catalog.affixes.end(),"Enabled Critical Chance definition missing");
        auto tier=std::find_if(affix->tiers.begin(),affix->tiers.end(),[](const AffixTier& t){return t.rank==normal_maximum_affix_tier;});
        require(tier!=affix->tiers.end() && tier->maximum==25,"Critical test requires T24 maximum 25%");
        item["affixes"]=Json::array({resolved_affix(*affix,*tier,tier->maximum,1)});
        item["next_affix_slot"]=2;item["development_fixture"]="guaranteed-critical-kit";
        validate_instance(item,true);effective_stats(item);return item;
    }
    const Json& lookup(const std::string& engine_id) const {return revisions.at(engine_id);}
    bool contains(const std::string& engine_id) const {return revisions.count(engine_id)!=0;}
    bool is_locked(const Json& item) const {
        const auto found=equipment_locks.find(item.at("instance_id"));
        return found==equipment_locks.end()?item.value("locked",false):found->second;
    }
    bool is_locked(const std::string& engine_id) const {return is_locked(lookup(engine_id));}
    void set_locked(const std::string& engine_id,bool locked) {
        equipment_locks[lookup(engine_id).at("instance_id").get<std::string>()]=locked;
    }
    void record(const Json& item) {
        validate_instance(item);effective_stats(item);
        std::string id=item.at("engine_id");
        require(!contains(id),"Revision identity already recorded");
        require(item.at("revision")==next_revision(item.at("instance_id")),"Unexpected item revision sequence");
        if (item.at("revision").get<int>()>1) {
            require(item.at("parent_engine_id").is_string(),"Missing edit parent");
            const auto& parent=lookup(item.at("parent_engine_id"));
            require(parent.at("instance_id")==item.at("instance_id"),"Edit changed logical identity");
            require(parent.at("base_snapshot")==item.at("base_snapshot") && parent.at("base_id")==item.at("base_id"),"Use explicit base migration to change a base");
        } else require(item.at("parent_engine_id").is_null(),"Fresh item cannot have edit parent");
        revisions.emplace(id,item);
        next_revisions[item.at("instance_id").get<std::string>()]=item.at("revision").get<int>()+1;
    }
    Json remove_affix(const std::string& engine_id,const std::string& affix_id) const {
        Json item=draft(engine_id);auto& list=item["affixes"];
        auto it=std::find_if(list.begin(),list.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(it!=list.end(),"Affix is not on the item");list.erase(it);
        validate_instance(item);return item;
    }
    Json add_affix(const std::string& engine_id,const std::string& affix_id,uint64_t seed) const {
        Json item=draft(engine_id);
        auto found=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a) {return a.id==affix_id;});
        require(found!=catalog.affixes.end(),"Affix definition missing");
        int damage=0;
        for(const auto& effect:item.at("affixes")) {
            require(effect.at("id")!=found->id,"Affix is already present");
            const auto existing=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a){return a.id==effect.at("id");});
            require(effect.at("group")!=found->group && (existing==catalog.affixes.end() || existing->group!=found->group),"Affix family is already present");
            if(direct_damage_stat(effect.at("stat"))) ++damage;
        }
        if(item.at("category")=="weapon") require(damage+direct_damage_stat(found->stat)<=lifetime_rules().at(item.at("rarity").get<std::string>()).maximum_prefixes,"Damage affix capacity exceeded");
        require(found->minimum_level<=item.at("item_level").get<int>() &&
            std::find(found->categories.begin(),found->categories.end(),item.at("category").get<std::string>())!=found->categories.end(),"Affix is ineligible for this instance");
        auto tiers=eligible_tiers(*found,item,100);require(!tiers.empty(),"No eligible tier");
        Random random(seed);const auto resolved=contextual_tier(*found,*tiers.front(),item_context(item));int value=quality_roll(random,resolved.minimum,resolved.maximum,1);
        int slot=item.value("next_affix_slot",1);
        for (const auto& effect:item.at("affixes")) slot=std::max(slot,effect.value("slot_id",0)+1);
        item["next_affix_slot"]=slot+1;item["affixes"].push_back(resolved_affix(*found,resolved,value,slot));
        validate_instance(item);effective_stats(item);return item;
    }
    Json reroll_affix(const std::string& engine_id,const std::string& affix_id,uint64_t seed) const {
        Json item=draft(engine_id);auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=effects.end(),"Affix not found for reroll");
        auto tier=reforge_tier(item,*target);Random random(seed);(*target)["tier"]=tier;
        (*target)["value"]=quality_roll(random,tier.at("minimum"),tier.at("maximum"),1);
        validate_instance(item);effective_stats(item);return item;
    }
    Json affix_tier(const Json& effect) const {
        validate_effect(effect,true);
        if (effect.contains("tier")) return effect.at("tier");
        // Historical prototype items have no tier metadata. Infer only for a
        // crafting preview/edit; loading never mutates or clamps that old item.
        const auto& source=definition(effect);const int value=effect.at("value");
        for (const auto& tier:source.tiers) if (value>=tier.minimum && value<=tier.maximum) return tier_snapshot(tier);
        throw std::invalid_argument("Legacy affix value has no compatible tier; explicit migration required");
    }
    Json reforge_tier(const Json& item,const Json& effect,int maximum_rank=100) const {
        const Json saved=affix_tier(effect);
        const auto found=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a){return a.id==effect.at("id");});
        if(found==catalog.affixes.end() || found->policy.value("minimum_roll_width",0)==0) return saved;
        // Old ten-tier snapshots remain authoritative. Attuning an old item
        // rerolls its original range rather than silently shrinking its power.
        if(!found->tiers.empty() && found->tiers.front().id.find("v24_t")==0 && saved.at("id").get<std::string>().find("v24_t")!=0) return saved;
        const bool universal=found->policy.value("universal_tier_ranges",false);
        if(!universal && saved.at("minimum")!=saved.at("maximum")) return saved;
        const auto& source=definition(effect);
        for(const auto* tier:eligible_tiers(source,item,maximum_rank)) if(tier->rank==saved.at("rank")) {
            const auto updated=item_tier(source,*tier,item);
            const int value=effect.at("value");
            if(universal || (value>=updated.at("minimum").get<int>() && value<=updated.at("maximum").get<int>())) return updated;
        }
        return saved; // Out-of-pool grandfathered rolls remain untouched.
    }
    int next_upgrade_rank(const Json& item,const Json& effect,int maximum_rank=100) const {
        const auto saved=affix_tier(effect);const auto& source=definition(effect);
        const bool old_scale=!source.tiers.empty() && source.tiers.front().id.find("v24_t")==0 &&
            saved.at("id").get<std::string>().find("v24_t")!=0;
        if(!old_scale) return saved.at("rank").get<int>()+1;
        // Explicit upgrades bridge an old frozen range into the new scale.
        // Choose the first legal band which starts at its old ceiling, so an
        // upgrade never rerolls a legacy item into a weaker numerical band.
        for(const auto* tier:eligible_tiers(source,item,maximum_rank)) {
            const auto resolved=item_tier(source,*tier,item);
            if(resolved.at("minimum")>=saved.at("maximum") && resolved.at("maximum")>saved.at("maximum")) return tier->rank;
        }
        return maximum_affix_tier+1;
    }
    Json replacement_options(const Json& item,const std::string& affix_id,int maximum_rank) const {
        validate_instance(item);require(maximum_rank>=1 && maximum_rank<=100,"Invalid replacement tier limit");
        const auto& effects=item.at("affixes");
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=effects.end(),"Selected replacement slot is not on the item");
        const auto old_tier=affix_tier(*target);
        const int success_limit=std::min(maximum_rank,old_tier.at("rank").get<int>());
        const int failure_limit=std::min(maximum_rank,std::max(1,old_tier.at("rank").get<int>()-1));
        Json result=Json::array();
        // Refuse the operation before charging if its failure path has no legal roll.
        if (replacements(item,*target,failure_limit).empty()) return result;
        for (auto candidate:replacements(item,*target,success_limit)) {
            auto tiers=eligible_tiers(*candidate,item,success_limit);
            auto tier=item_tier(*candidate,*tiers.back(),item);
            result.push_back({{"id",candidate->id},{"label",candidate->label},{"kind",candidate->kind},
                {"group",candidate->group},{"unit",candidate->unit},{"tier",tier},
                {"forge_category",candidate->forge_category}});
        }
        return result;
    }
    Json replace_affix(const std::string& engine_id,const std::string& affix_id,const std::string& chosen_id,
        AffixReplacementResult outcome,uint64_t seed,int quality_draws,int maximum_rank) const {
        const auto& old=lookup(engine_id);
        require(quality_draws>=1 && quality_draws<=16,"Invalid replacement roll quality");
        require(outcome==AffixReplacementResult::Failure || outcome==AffixReplacementResult::Success ||
            outcome==AffixReplacementResult::Perfect,"Unknown replacement result");
        auto options=replacement_options(old,affix_id,maximum_rank);
        auto chosen=std::find_if(options.begin(),options.end(),[&](const Json& option) {return option.at("id")==chosen_id;});
        require(chosen!=options.end(),"Chosen replacement affix is ineligible for the targeted slot");
        Json item=draft(engine_id);auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        const auto old_tier=affix_tier(*target);Random random(seed);
        const Affix* selected=nullptr;Json tier;bool maximize=false;
        if (outcome==AffixReplacementResult::Failure) {
            const int failure_limit=std::min(maximum_rank,std::max(1,old_tier.at("rank").get<int>()-1));
            auto candidates=replacements(item,*target,failure_limit);uint64_t total=0;
            for (auto candidate:candidates) total+=candidate->weight;
            auto choice=random.below(total);
            for (auto candidate:candidates) {if (choice<candidate->weight) {selected=candidate;break;}choice-=candidate->weight;}
            require(selected!=nullptr,"Failed replacement selection has no eligible family");
            auto tiers=eligible_tiers(*selected,item,failure_limit);
            tier=item_tier(*selected,*tiers.at(static_cast<size_t>(random.below(tiers.size()))),item);
            quality_draws=1; // Failure has a uniform value roll, with no success bias.
        } else {
            auto candidate=std::find_if(catalog.affixes.begin(),catalog.affixes.end(),[&](const Affix& a) {return a.id==chosen_id;});
            require(candidate!=catalog.affixes.end(),"Chosen replacement definition disappeared");selected=&*candidate;
            tier=chosen->at("tier");
            if (outcome==AffixReplacementResult::Perfect) {
                auto tiers=eligible_tiers(*selected,item,maximum_rank);
                auto next=std::find_if(tiers.begin(),tiers.end(),[&](const AffixTier* t) {return t->rank==tier.at("rank").get<int>()+1;});
                if (next!=tiers.end()) tier=item_tier(*selected,**next,item);
                else maximize=true;
            }
        }
        int slot=target->value("slot_id",0);
        if (slot==0) {
            slot=item.value("next_affix_slot",1);
            for (const auto& effect:effects) slot=std::max(slot,effect.value("slot_id",0)+1);
            item["next_affix_slot"]=slot+1;
        }
        const int value=maximize?tier.at("maximum").get<int>():quality_roll(random,tier.at("minimum"),tier.at("maximum"),quality_draws);
        AffixTier snapshot;snapshot.id=tier.at("id");snapshot.rank=tier.at("rank");snapshot.label=tier.at("label");
        snapshot.minimum=tier.at("minimum");snapshot.maximum=tier.at("maximum");snapshot.minimum_level=tier.at("minimum_level");
        *target=resolved_affix(*selected,snapshot,value,slot);
        // Preserve resolved-tier provenance from the replacement preview;
        // bounds/values are already effective and are never scaled in combat.
        (*target)["tier"]=tier;
        validate_instance(item);effective_stats(item);
        return item.at("affixes")==old.at("affixes")?old:item;
    }
    Json craft_tier_options(const Json& item,const std::string& affix_id,int maximum_rank) const {
        validate_instance(item);require(maximum_rank>=1 && maximum_rank<=100,"Invalid crafting tier limit");
        const auto& effects=item.at("affixes");
        const auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=effects.end(),"Selected crafting slot is not on the item");
        Json result=Json::array();
        for(const auto* tier:eligible_tiers(definition(*target),item,maximum_rank)) result.push_back(item_tier(definition(*target),*tier,item));
        return result;
    }
    Json reroll_tier_weighted(const std::string& engine_id,const std::string& affix_id,
        const std::map<int,uint32_t>& weights,uint64_t seed,int quality_draws,int maximum_rank) const {
        const auto& old=lookup(engine_id);
        require(can_craft(old,affix_id,"reroll_tier",maximum_rank),"Tier reroll is unavailable");
        require(!weights.empty() && quality_draws>=1 && quality_draws<=16,"Invalid performance tier roll");
        const auto legal=craft_tier_options(old,affix_id,maximum_rank);uint64_t total=0;
        for(const auto& weight:weights) {
            require(weight.second>0 && weight.second<=1000000 &&
                std::any_of(legal.begin(),legal.end(),[&](const Json& tier) {return tier.at("rank")==weight.first;}),"Performance tier roll exceeds eligibility");
            total+=weight.second;
        }
        Random random(seed);int strongest=0;
        for(int i=0;i<quality_draws;++i) {
            auto selection=random.below(total);
            for(const auto& weight:weights) {if(selection<weight.second) {strongest=std::max(strongest,weight.first);break;}selection-=weight.second;}
        }
        Json item=draft(engine_id);auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        auto selected=std::find_if(legal.begin(),legal.end(),[&](const Json& tier) {return tier.at("rank")==strongest;});
        require(selected!=legal.end(),"Performance tier selection failed");
        int slot=target->value("slot_id",0);
        if(slot==0) {slot=item.value("next_affix_slot",1);for(const auto& effect:effects) slot=std::max(slot,effect.value("slot_id",0)+1);item["next_affix_slot"]=slot+1;}
        (*target)["tier"]=*selected;(*target)["slot_id"]=slot;
        (*target)["value"]=quality_roll(random,selected->at("minimum"),selected->at("maximum"),quality_draws);
        validate_instance(item);effective_stats(item);
        return item.at("affixes")==old.at("affixes")?old:item;
    }
    bool can_craft(const Json& item,const std::string& affix_id,const std::string& operation,int maximum_rank) const {
        validate_instance(item);require(maximum_rank>=1 && maximum_rank<=100,"Invalid crafting tier limit");
        auto target=std::find_if(item.at("affixes").begin(),item.at("affixes").end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=item.at("affixes").end(),"Selected affix is not on the item");
        const auto tier=affix_tier(*target);
        if (operation=="reroll_value") {const auto editable=reforge_tier(item,*target,maximum_rank);return editable.at("minimum")!=editable.at("maximum");}
        if (operation=="replace_affix") return !replacement_options(item,affix_id,maximum_rank).empty();
        const auto& source=definition(*target);auto tiers=eligible_tiers(source,item,maximum_rank);
        if (operation=="reroll_tier") return tiers.size()>1;
        if (operation=="upgrade_tier") {const int next=next_upgrade_rank(item,*target,maximum_rank);return std::any_of(tiers.begin(),tiers.end(),[&](const AffixTier* t) {return t->rank==next;});}
        throw std::invalid_argument("Unknown crafting operation");
    }
    Json reforge_value(const std::string& engine_id,const std::string& affix_id,int value) const {
        const auto& old=lookup(engine_id);Json item=draft(engine_id);auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        require(target!=effects.end(),"Value reforging target is missing");
        const auto tier=reforge_tier(old,*target);
        require(value>=tier.at("minimum").get<int>() && value<=tier.at("maximum").get<int>(),"Reforged value exceeds current tier");
        int slot=target->value("slot_id",0);
        if(slot==0) {slot=item.value("next_affix_slot",1);for(const auto& effect:effects) slot=std::max(slot,effect.value("slot_id",0)+1);item["next_affix_slot"]=slot+1;}
        (*target)["tier"]=tier;(*target)["slot_id"]=slot;(*target)["value"]=value;
        validate_instance(item);effective_stats(item);
        return item.at("affixes")==old.at("affixes")?old:item;
    }
    Json craft(const std::string& engine_id,const std::string& affix_id,const std::string& operation,
        uint64_t seed,int quality_draws,int maximum_rank) const {
        require(operation!="replace_affix","Replacement requires an explicit chosen affix and forging result");
        const auto& old=lookup(engine_id);require(can_craft(old,affix_id,operation,maximum_rank),"Selected crafting operation is unavailable");
        require(quality_draws>=1 && quality_draws<=16,"Invalid crafting quality");
        Json item=draft(engine_id);auto& effects=item["affixes"];
        auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==affix_id;});
        Random random(seed);Json tier=operation=="reroll_value"?reforge_tier(item,*target,maximum_rank):affix_tier(*target);
        int slot=target->value("slot_id",0);
        if (slot==0) {
            slot=item.value("next_affix_slot",1);
            for (const auto& effect:effects) slot=std::max(slot,effect.value("slot_id",0)+1);
            item["next_affix_slot"]=slot+1;
        }
        {
            if (operation!="reroll_value") {
                auto tiers=eligible_tiers(definition(*target),item,maximum_rank);
                if (operation=="upgrade_tier") {
                    const int next=next_upgrade_rank(item,*target,maximum_rank);
                    auto chosen=std::find_if(tiers.begin(),tiers.end(),[&](const AffixTier* t) {return t->rank==next;});
                    require(chosen!=tiers.end(),"Next tier is locked");tier=item_tier(definition(*target),**chosen,item);
                } else {
                    int strongest=0;
                    for (int draw=0;draw<quality_draws;++draw) strongest=std::max(strongest,static_cast<int>(random.below(tiers.size())));
                    tier=item_tier(definition(*target),*tiers.at(strongest),item);
                }
            }
            (*target)["tier"]=tier;(*target)["slot_id"]=slot;
            (*target)["value"]=quality_roll(random,tier.at("minimum"),tier.at("maximum"),quality_draws);
        }
        validate_instance(item);effective_stats(item);
        return item.at("affixes")==old.at("affixes")?old:item;
    }
    Json finish_fresh(const Json& generated,uint64_t seed,int quality_draws) const {
        validate_instance(generated,true);
        require(generated.at("revision")==1 && generated.at("parent_engine_id").is_null() && next_revision(generated.at("instance_id"))==1,"Fresh quality roll requires an unissued draft");
        Json result=generated;Random random(seed);
        for (auto& effect:result["affixes"]) {
            auto tier=affix_tier(effect);effect["tier"]=tier;
            effect["value"]=quality_roll(random,tier.at("minimum"),tier.at("maximum"),quality_draws);
        }
        validate_instance(result,true);return result;
    }
    Json upgrade_rarity(const std::string& engine_id,const std::string& rarity) const {
        Json item=draft(engine_id);const auto& rules=lifetime_rules();
        require(rules.count(rarity)!=0 && rules.at(rarity).total>rules.at(item.at("rarity")).total,"Rarity must increase");
        item["rarity"]=rarity;validate_instance(item);return item;
    }
    Json enhance_affix(const std::string& engine_id,const std::string& affix_id,int amount,int ceiling) const {
        // Forge/crafting policy supplies its configured step and ceiling. This
        // shared lifetime operation changes one resolved value, never its base,
        // group, side, implicit, or logical identity. It does not spend resources.
        Json item=draft(engine_id);enhance_values(item,affix_id,amount,ceiling);return item;
    }
    Json enhance_fresh(const Json& generated,const std::string& affix_id,int amount,int ceiling) const {
        validate_instance(generated,true);
        require(generated.at("revision")==1 && generated.at("parent_engine_id").is_null() &&
            next_revision(generated.at("instance_id"))==1,"Fresh enhancement requires an unissued generated draft");
        Json item=generated;enhance_values(item,affix_id,amount,ceiling);return item;
    }
    Json set_implicits(const std::string& engine_id,const Json& implicits) const {
        Json item=draft(engine_id);item["implicits"]=implicits;validate_instance(item);effective_stats(item);return item;
    }
    Json upgrade_level(const std::string& engine_id,int level) const {
        Json item=draft(engine_id);
        require(level>item.at("item_level").get<int>(),"Item level must increase");
        item["item_level"]=level;validate_instance(item);return item;
    }
    Json export_history() const {
        Json list=Json::array();
        // Restore in per-item revision order, regardless of lexicographic R_10/R_2.
        std::vector<Json> sorted;for (const auto& row:revisions) sorted.push_back(row.second);
        std::sort(sorted.begin(),sorted.end(),[](const Json& a,const Json& b) {
            if (a.at("instance_id")!=b.at("instance_id")) return a.at("instance_id")<b.at("instance_id");
            return a.at("revision").get<int>()<b.at("revision").get<int>();
        });
        for (const auto& item:sorted) list.push_back(item);
        Json history={{"schema_version",1},{"revisions",list}};
        if(!equipment_locks.empty())history["equipment_locks"]=equipment_locks;
        return history;
    }
    void import_history(const Json& data) {
        require(data.at("schema_version")==1 && data.at("revisions").is_array(),"Unsupported item history");
        // Validate into a temporary store; a corrupt restore cannot partially mutate state.
        ItemService replacement(bases,catalog);
        for (const auto& item:data.at("revisions")) replacement.record(item);
        const auto locks=data.value("equipment_locks",Json::object());require(locks.is_object(),"Invalid equipment lock state");
        for(const auto& p:locks.items()) {
            require(p.value().is_boolean() && replacement.next_revisions.count(p.key()),"Invalid equipment lock identity");
            replacement.equipment_locks.emplace(p.key(),p.value().get<bool>());
        }
        revisions=std::move(replacement.revisions);
        next_revisions=std::move(replacement.next_revisions);
        equipment_locks=std::move(replacement.equipment_locks);
    }
};
}
