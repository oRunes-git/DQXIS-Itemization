#pragma once
#include "item_lifecycle.hpp"
#include <unordered_set>

namespace itemization {
struct CombatBalance {
    Json caps={{"damage_increase",300},{"resistance",80},{"physical_reduction",60},{"spell_reduction",60},
        {"mp_cost",60},{"healing_power",100},{"healing_received",100},{"item_healing",100},{"status_resistance",90},{"life_steal",25}};
    Json policy={{"elemental_crit_multiplier",2.0},{"regen_hp_fraction_cap",0.08},{"regen_mp_fraction_cap",0.05},{"life_steal_action_hp_fraction_cap",0.15}};
    static CombatBalance parse(const Json& catalog) {
        CombatBalance rules;
        if(catalog.contains("total_caps")) rules.caps.merge_patch(catalog.at("total_caps"));
        if(catalog.contains("combat_policy")) rules.policy.merge_patch(catalog.at("combat_policy"));
        for(auto i=rules.caps.begin();i!=rules.caps.end();++i) require(i.value().is_number() && std::isfinite(i.value().get<double>()) && i.value().get<double>()>=0 &&
            i.value().get<double>()<=(i.key()=="damage_increase"?1000:i.key()=="mp_absorption_bonus"?1000:100),"Invalid aggregate combat cap");
        for(const auto key:{"regen_hp_fraction_cap","regen_mp_fraction_cap","life_steal_action_hp_fraction_cap"}) {
            const double value=rules.policy.at(key);require(std::isfinite(value) && value>=0 && value<=1,"Invalid recovery budget");
        }
        const double crit=rules.policy.at("elemental_crit_multiplier");require(std::isfinite(crit) && crit>=1 && crit<=10,"Invalid elemental critical multiplier");
        return rules;
    }
    double cap(const char* id) const {return caps.at(id);}
};
// Portable execution model. Native adapters must supply pre-resistance component
// amounts and authoritative action identity; never recalculate a second attack.
struct CombatAffixes {
    std::map<std::string,double> values;
    double get(const std::string& id) const {auto i=values.find(id);return i==values.end()?0:i->second;}
    static CombatAffixes collect(const std::vector<Json>& equipment,const std::string& origin_weapon) {
        CombatAffixes result;
        for(const auto& item:equipment) {
            validate_instance(item);
            for(const auto key:{"implicits","affixes"}) for(const auto& effect:item.at(key)) {
                const auto policy=effect.value("effect_policy",Json::object());
                const std::string stat=effect.at("stat");
                const std::string default_scope=(stat.rfind("added_",0)==0 && stat.rfind("added_spell_",0)!=0) || stat.rfind("skill_damage_",0)==0?"weapon_local":"wearer";
                const std::string scope=policy.value("scope",default_scope);
                const bool weapon=item.at("category")=="weapon";
                if((scope=="weapon_local" || (scope=="origin_weapon_or_wearer" && weapon)) && item.at("engine_id")!=origin_weapon) continue;
                if(custom_stat(stat)) result.values[stat]+=effect_value(item,effect);
            }
        }
        return result;
    }
};
struct DamageComponent {std::string element="physical";double raw=0;bool added=false;};
struct CombatHit {
    bool landed=true,guarded=false,invulnerable=false,metal=false,physical=true,spell=false,critical=false;
    bool weapon_hit=true,full_hp=false,low_hp=false,target_poisoned=false,target_debuffed=false;
    bool native_components_resisted=false;
    int expected_hits=1,native_restricted_damage=0,native_limit=9999;
    // Complete multiplier for this particular hit, supplied by the native
    // adapter: action, repeat-hit, hand and target modifiers applied once.
    // Hit count is metadata, not an automatic divisor for added damage.
    double added_damage_coefficient=1,elemental_critical_multiplier=0;
    std::string skill_family,enemy_family;
    std::vector<DamageComponent> components;
    std::map<std::string,double> target_element_multipliers;
};
struct CombatDamage {int damage=0;double amount=0;std::vector<DamageComponent> resolved;};
// Eligibility is semantic and shared with the native resistance probe. Even a
// stale weapon_hit flag cannot route attack-only affixes into a spell.
inline double added_element_amount(const CombatHit& hit,const CombatAffixes& effects,const std::string& element) {
    if(hit.spell) return effects.get("added_spell_"+element);
    return hit.weapon_hit?effects.get("added_"+element):0;
}
inline double capped(double value,double cap) {require(std::isfinite(value) && value>=0,"Invalid combat bonus");return std::min(value,cap);}
inline CombatDamage resolve_affix_damage(const CombatHit& hit,const CombatAffixes& attack,const CombatAffixes& defence,const CombatBalance& balance={}) {
    require(hit.expected_hits>=1 && hit.expected_hits<=100 && std::isfinite(hit.added_damage_coefficient) && hit.added_damage_coefficient>=0 &&
        hit.added_damage_coefficient<=100 && hit.native_limit>=0 && hit.native_limit<=100000 && std::isfinite(hit.elemental_critical_multiplier) &&
        (hit.elemental_critical_multiplier==0 || (hit.elemental_critical_multiplier>=1 && hit.elemental_critical_multiplier<=10)),"Invalid native damage context");
    CombatDamage result;
    if(!hit.landed || hit.guarded || hit.invulnerable) return result;
    if(hit.metal) {
        // Metal's ordinary/anti-metal result is a native exception, not one
        // chip per element. Physical criticals keep the native defence bypass.
        double amount=hit.native_components_resisted && !hit.components.empty()?hit.components.front().raw:hit.native_restricted_damage;
        if(hit.physical && hit.critical) amount*=1+capped(attack.get("critical_damage"),balance.cap("damage_increase"))/100;
        result.amount=std::clamp(amount,0.0,double(hit.native_limit));
        result.damage=static_cast<int>(std::floor(result.amount));return result;
    }
    auto components=hit.components;
    for(const std::string element:{"fire","ice","wind","earth","lightning","light","dark"}) {
        const double added=added_element_amount(hit,attack,element)*hit.added_damage_coefficient;
        if(added>0) components.push_back({element,added,true});
    }
    double total=0;
    for(auto component:components) {
        require(std::isfinite(component.raw) && component.raw>=0,"Invalid pre-mitigation component");
        double bonus=0;
        if(hit.spell) bonus+=attack.get("spell_damage");
        if(hit.physical && hit.critical) bonus+=attack.get("critical_damage");
        if(hit.weapon_hit && !hit.skill_family.empty()) bonus+=attack.get("skill_damage_"+hit.skill_family);
        if(!hit.enemy_family.empty()) bonus+=attack.get("damage_family_"+hit.enemy_family);
        if(hit.weapon_hit) {
            if(hit.full_hp) bonus+=attack.get("conditional_damage_full_hp");
            if(hit.low_hp) bonus+=attack.get("conditional_damage_low_hp");
            if(hit.target_poisoned) bonus+=attack.get("conditional_damage_poisoned");
            if(hit.target_debuffed) bonus+=attack.get("conditional_damage_debuffed");
            bonus+=attack.get("evasion_momentum");
        }
        if(component.element!="physical") bonus+=attack.get("damage_"+component.element);
        double amount=component.raw*(1+capped(bonus,balance.cap("damage_increase"))/100);
        // Native components arrive with their native critical treatment already
        // applied, but before elemental mitigation. Added components share that
        // one critical decision and use their explicit elemental multiplier.
        if(component.added && hit.critical) amount*=hit.elemental_critical_multiplier>0?hit.elemental_critical_multiplier:balance.policy.at("elemental_crit_multiplier").get<double>();
        if(component.element!="physical" && (component.added || !hit.native_components_resisted)) {
            auto i=hit.target_element_multipliers.find(component.element);
            require(i!=hit.target_element_multipliers.end() && std::isfinite(i->second) && i->second>=0 && i->second<=10,"Missing elemental resistance context");
            double multiplier=i->second;
            const double floor=1-balance.cap("resistance")/100;
            if(multiplier>0) multiplier=std::max(std::min(floor,multiplier),multiplier*(1-capped(defence.get("resistance_"+component.element),balance.cap("resistance"))/100));
            amount*=multiplier; // Immunity stays zero, including criticals.
        }
        if(hit.physical) amount*=1-capped(defence.get("physical_reduction"),balance.cap("physical_reduction"))/100;
        if(hit.spell) amount*=1-capped(defence.get("spell_reduction"),balance.cap("spell_reduction"))/100;
        component.raw=amount;total+=amount;result.resolved.push_back(component);
    }
    require(std::isfinite(total),"Mixed damage overflow");
    result.amount=std::min(total,double(hit.native_limit));
    result.damage=static_cast<int>(std::floor(result.amount));
    return result;
}
inline int affix_mp_cost(int native_cost,const CombatAffixes& effects,bool eligible=true,const CombatBalance& balance={}) {
    require(native_cost>=0,"Invalid MP cost");
    return eligible?static_cast<int>(std::ceil(native_cost*(1-capped(effects.get("mp_cost"),balance.cap("mp_cost"))/100))):native_cost;
}
inline int affix_healing(int amount,const CombatAffixes& source,const CombatAffixes& target,bool item=false,bool triggered=false,int limit=9999,const CombatBalance& balance={}) {
    require(amount>=0 && limit>=0 && limit<=100000,"Invalid healing context");
    if(triggered) return std::min(amount,limit);
    const double bonus=capped(source.get(item?"item_healing":"healing_power"),balance.cap(item?"item_healing":"healing_power"));
    const double received=capped(target.get("healing_received"),balance.cap("healing_received"));
    return static_cast<int>(std::floor(std::min(double(limit),amount*(1+bonus/100)*(1+received/100))));
}
inline double affix_status_chance(double native_chance,const CombatAffixes& target,const std::string& status,bool immune,const CombatBalance& balance={}) {
    require(std::isfinite(native_chance) && native_chance>=0 && native_chance<=100,"Invalid native status chance");
    return immune?0:native_chance*(1-capped(target.get("status_resistance_"+status),balance.cap("status_resistance"))/100);
}
struct AffixResources {int hp=0,mp=0,max_hp=0,max_mp=0;};
struct AffixRecovery {int hp=0,mp=0;};
struct AffixEvent {
    uint64_t battle=0,turn=0,action=0,target=0,hit=0;
    std::string actor;
    bool landed=false,physical=false,weapon_hit=false,critical=false,blocked=false,dodged=false,defeated=false,turn_end=false;
    bool equipment_triggered=false,preview=false,metal=false;
    int actual_hp_damage=0;
};
class AffixEventExecutor {
    CombatBalance balance;
    // Battle-scoped deduplication; previews and triggered effects never mutate it.
    uint64_t battle=0;std::set<std::string> delivered;
    std::map<std::string,uint64_t> momentum_expiry;
    std::map<std::string,int> life_steal_used;
    static std::string key(const AffixEvent& e,const std::string& effect,bool turn=false) {
        return e.actor+"/"+effect+"/"+std::to_string(turn?e.turn:e.action)+"/"+std::to_string(turn?0:e.target);
    }
public:
    explicit AffixEventExecutor(CombatBalance rules={}):balance(std::move(rules)) {}
    void reset(uint64_t id) {battle=id;delivered.clear();momentum_expiry.clear();life_steal_used.clear();}
    bool once(const AffixEvent& e,const std::string& effect,bool turn=false) {return delivered.insert(key(e,effect,turn)).second;}
    bool consume_momentum(const AffixEvent& e) {
        if(e.preview || e.equipment_triggered) return false;
        if(e.battle!=battle) reset(e.battle);
        auto i=momentum_expiry.find(e.actor);if(i==momentum_expiry.end()) return false;
        const bool active=e.turn<=i->second;momentum_expiry.erase(i);return active;
    }
    AffixRecovery recovery(const AffixEvent& e,const CombatAffixes& effects,const AffixResources& resources) {
        AffixRecovery result;
        if(e.preview || e.equipment_triggered) return result;
        if(e.battle!=battle) reset(e.battle);
        require(resources.hp>=0 && resources.mp>=0 && resources.max_hp>=resources.hp && resources.max_mp>=resources.mp &&
            e.actual_hp_damage>=0,"Invalid resource context");
        if(resources.hp==0) return result; // Equipment recovery cannot resurrect.
        auto add=[&](const char* id,bool condition,bool turn=false){
            if(condition && effects.get(id)>0 && once(e,id,turn)) {
                const int amount=static_cast<int>(std::floor(effects.get(id)));
                (std::string(id).rfind("hp_",0)==0?result.hp:result.mp)+=amount;
            }
        };
        add("hp_turn",e.turn_end,true);add("mp_turn",e.turn_end,true);
        if(e.turn_end) {
            result.hp=std::min(result.hp,static_cast<int>(std::floor(resources.max_hp*balance.policy.at("regen_hp_fraction_cap").get<double>())));
            result.mp=std::min(result.mp,static_cast<int>(std::floor(resources.max_mp*balance.policy.at("regen_mp_fraction_cap").get<double>())));
        }
        auto action_event=e;action_event.target=0;
        for(const std::string id:{"hp_defeat","mp_defeat","hp_on_block"}) {
            const bool condition=id=="hp_on_block"?e.blocked:e.defeated;
            if(condition && effects.get(id)>0 && once(action_event,id))
                (id.rfind("hp_",0)==0?result.hp:result.mp)+=static_cast<int>(effects.get(id));
        }
        // Crit MP is once per action across all targets; blocking once per incoming action.
        if(e.critical && e.landed && e.physical && once(action_event,"crit_mp")) result.mp+=static_cast<int>(effects.get("mp_on_crit"));
        if(e.landed && e.physical && !e.metal && e.actual_hp_damage>0 && once(e,"life_steal_hit_"+std::to_string(e.hit))) {
            const auto action_key=key(action_event,"life_steal_budget");
            int& used=life_steal_used[action_key];
            const int amount=std::min(static_cast<int>(std::floor(e.actual_hp_damage*capped(effects.get("life_steal"),balance.cap("life_steal"))/100)),
                std::max(0,static_cast<int>(std::floor(resources.max_hp*balance.policy.at("life_steal_action_hp_fraction_cap").get<double>()))-used));
            used+=amount;result.hp+=amount;
        }
        if(e.dodged && effects.get("evasion_momentum")>0) momentum_expiry[e.actor]=e.turn+1;
        result.hp=std::min(result.hp,resources.max_hp-resources.hp);
        result.mp=std::min(result.mp,resources.max_mp-resources.mp);return result;
    }
    std::vector<std::string> ailments(const AffixEvent& e,const CombatAffixes& effects,
        const std::map<std::string,double>& target_multipliers,const std::set<std::string>& immunities,Random& random) {
        std::vector<std::string> result;
        if(e.preview || e.equipment_triggered || !e.landed || !e.physical || !e.weapon_hit) return result;
        if(e.battle!=battle) reset(e.battle);
        for(const std::string status:{"poison","sleep","defence_down"}) {
            const std::string id="on_hit_"+status;
            if(effects.get(id)<=0 || immunities.count(status) || !once(e,id)) continue;
            auto i=target_multipliers.find(status);require(i!=target_multipliers.end() && std::isfinite(i->second) && i->second>=0 && i->second<=10,"Missing native ailment resistance");
            const double chance=std::min(100.0,effects.get(id)*i->second);
            if(random.below(1000000)<static_cast<uint64_t>(chance*10000)) result.push_back(status);
        }
        return result;
    }
};
} // namespace itemization
