#pragma once
#include "combat_affixes.hpp"

namespace itemization {
inline bool native_fire_weapon_eligible(const std::string& category,const std::string& type,uint8_t region) {
    return category=="weapon" && type=="Greatsword" && region==1;
}
// First live proof deliberately covers one weapon family and ordinary hits.
// Unsupported contexts retain vanilla damage until their adapters are audited.
inline bool native_fire_prototype_eligible(const std::string& action,const Json& tags,
    bool critical,bool metal,int weapons,int hit_groups,int hits,bool override_row) {
    // Motion, distance and action spelling never decide combat eligibility.
    // The runtime separately validates the actual source weapon family.
    return !action.empty() && tags.is_object() && tags.value("physical",false) &&
        !tags.value("spell",false) && tags.value("action_category",std::string{})=="EJackActionCategory__Kougeki" &&
        tags.value("ordinary_weapon_damage",false) && tags.value("battle",false) &&
        tags.value("target",std::string{})=="EJackActionTargetType__Foe" &&
        tags.value("extent",std::string{})=="EJackActionExtentType__Single" &&
        tags.value("damage_type",std::string{})=="EJackActionDamageType__DamageHP" &&
        tags.value("multi_attack_id",std::string{})=="None" &&
        !critical && !metal && weapons==1 && hit_groups>=0 && hit_groups<=1 && hits>=0 && hits<=1 && !override_row;
}
inline float native_element_merge(float native_component,float added_component) {
    require(std::isfinite(native_component) && native_component>=0 && native_component<=100000 &&
        std::isfinite(added_component) && added_component>=0 && added_component<=100000,
        "Invalid native elemental component");
    return native_component+added_component;
}
} // namespace itemization
