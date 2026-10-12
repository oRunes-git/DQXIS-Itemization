#pragma once
#include "combat_affixes.hpp"

namespace itemization {
inline Json resolve_native_action_tags(const Json& actions,const Json& damage_rows,
    const std::string& name,const std::string& override_row) {
    const auto found=actions.find(name);
    if(found==actions.end() || !found->is_object()) return Json();
    auto resolved=*found;
    if(override_row.empty() || override_row=="None" || override_row==resolved.value("damage_row",std::string{})) return resolved;
    const auto row=damage_rows.find(override_row);
    if(row==damage_rows.end() || !row->is_object()) return Json();
    for(const auto* key:{"damage_type","physical","damage_limit","fixed_damage","ordinary_weapon_damage"})
        resolved[key]=row->at(key);
    for(const auto* key:{"calculation_type","off_hand"}) if(row->contains(key)) resolved[key]=row->at(key);
    resolved["damage_row"]=override_row;return resolved;
}
struct NativeWeaponSlot {std::string id,category;uint8_t region=0;};
struct NativeCriticalOrigin {bool valid=false;std::string weapon;};
inline NativeCriticalOrigin native_critical_origin(const std::vector<NativeWeaponSlot>& slots) {
    NativeCriticalOrigin result;int count=0;
    for(const auto& slot:slots) {
        if((slot.region!=1 && slot.region!=2) || slot.id.empty() || slot.id=="None") continue;
        if(slot.category!="weapon" && slot.category!="shield") return {};
        if(slot.category=="weapon") {++count;if(slot.region==1) result.weapon=slot.id;}
    }
    result.valid=count==1 && !result.weapon.empty();return result;
}
// Applies a pure multiplier to an already resolved native result. No additional
// attack, critical roll, elemental stage, healing event or resource write occurs.
struct NativeScalarProjection {
    int before=0,after=0,limit=0;
    double bonus=0;
    std::string stat,reason="ineligible";
    bool changed() const {return before!=after;}
};
inline NativeScalarProjection native_scalar_projection(int amount,uint8_t damage_type,
    const Json& action,bool blocked,const CombatAffixes& source,const CombatBalance& balance={},bool critical=false,bool metal=false) {
    require(amount>=0 && amount<=100000,"Invalid native scalar amount");
    NativeScalarProjection result;result.before=result.after=amount;
    if(blocked || amount==0 || !action.is_object()) return result;
    const std::string type=action.value("damage_type",std::string{}),category=action.value("action_category",std::string{});
    const bool spell=category=="EJackActionCategory__Jumon",skill=category=="EJackActionCategory__Tokugi";
    if(damage_type==1 && type=="EJackActionDamageType__DamageHP" && critical && !metal && amount>1 &&
        action.value("physical",false) && !action.value("spell",false) &&
        (category=="EJackActionCategory__Kougeki" || skill) &&
        action.value("critical_type",std::string{})=="EJackActionCriticalType__KougekiTantai") {
        // The engine already calculated this critical. Its ordinary attack rows
        // carry fixed_damage too; that flag must not exclude actual criticals.
        result.stat="critical_damage";result.bonus=capped(source.get(result.stat),balance.cap("damage_increase"));
    } else if(damage_type==1 && type=="EJackActionDamageType__DamageHP" && spell) {
        // Preserve native one-point exceptions until their metal/immunity paths
        // have separate live verification. Fixed damage is not a scalable spell.
        if(amount<=1 || action.value("fixed_damage",false)) return result;
        result.stat="spell_damage";result.bonus=capped(source.get(result.stat),balance.cap("damage_increase"));
    } else if(damage_type==3 && type=="EJackActionDamageType__HealHP" && (spell || skill)) {
        result.stat="healing_power";result.bonus=capped(source.get(result.stat),balance.cap("healing_power"));
    } else return result;
    result.limit=action.value("damage_limit",0);
    if(result.limit<1 || result.limit>100000 || amount>result.limit) {result.reason="unsupported_native_limit";return result;}
    result.after=static_cast<int>(std::floor(std::min(double(result.limit),amount*(1+result.bonus/100))));
    result.reason=result.bonus==0?"no_modifier":result.changed()?"scaled":"rounded_or_capped";
    return result;
}
} // namespace itemization
