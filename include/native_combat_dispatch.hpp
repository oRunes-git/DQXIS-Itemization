#pragma once
#include "native_combat_scalars.hpp"

namespace itemization {
inline bool native_offensive_spell(const Json& action,uint8_t row_type) {
    // Damage row overrides and damaging children retain their own action
    // category, while nondamaging spell/parent rows confer no bonus hit.
    return row_type==1 && action.is_object() && action.value("spell",false);
}
inline double native_spell_critical_factor(double input,double output,int chosen_percent,double zero_factor=1) {
    require(std::isfinite(input) && input>=0 && std::isfinite(output) && output>=0,"Invalid native spell critical amount");
    const double factor=chosen_percent?chosen_percent/100.0:input>0?output/input:zero_factor;
    require(std::isfinite(factor) && factor>=1 && factor<=10,"Invalid native spell critical multiplier");
    return factor;
}
// Selection is per resolved damage row, never inferred from animation or the
// number of equipped weapons. A shield in the off hand is not an origin weapon.
inline NativeCriticalOrigin native_hit_origin(const std::vector<NativeWeaponSlot>& slots,bool off_hand) {
    NativeCriticalOrigin result;
    for(const auto& slot:slots) if(slot.region==(off_hand?2:1) && slot.category=="weapon" && !slot.id.empty() && slot.id!="None") {
        require(!result.valid,"Multiple weapons in one native hand");result={true,slot.id};
    }
    return result;
}
inline double native_resistance_projection(double native_percent,double affix_percent,double cap) {
    require(std::isfinite(native_percent) && native_percent>=0 && native_percent<=1000,"Invalid native susceptibility");
    if(native_percent==0) return 0; // Native immunity wins.
    const double remaining=native_percent*(1-capped(affix_percent,cap)/100);
    // Equipment may not turn a stronger native resistance into a weaker one.
    return std::max(std::min(native_percent,100-cap),remaining);
}
struct NativeHitScale {
    double coefficient=1;
    bool valid=true;
    void multiply(double value) {
        if(!std::isfinite(value) || value<0 || value>100 || !std::isfinite(coefficient*value) || coefficient*value>100) {valid=false;return;}
        coefficient*=value;
    }
};
// Original components have already passed the engine's resistance stage. The
// adapter resolves added components separately, then calls native rounding once.
inline double native_damage_amount(double amount,const CombatHit& context,const CombatAffixes& source,
    const CombatAffixes& target,const CombatBalance& balance={}) {
    require(std::isfinite(amount) && amount>=0,"Invalid native terminal amount");
    auto hit=context;hit.native_components_resisted=true;
    hit.components={{context.components.empty()?"physical":context.components.front().element,amount,false}};
    return resolve_affix_damage(hit,source,target,balance).amount;
}
} // namespace itemization
