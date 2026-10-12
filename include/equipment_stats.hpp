#pragma once
#include "itemization.hpp"
#include <array>
#include <cmath>
#include <cstring>

namespace itemization {
struct EquipmentStatBinding {const char* stat;size_t offset;int parameter;bool percentage;};
inline const std::array<EquipmentStatBinding,17>& equipment_stat_bindings() {
    static const std::array<EquipmentStatBinding,17> bindings{{
        {"attack",0x98,1,false},{"defence",0x9C,2,false},{"mp_absorption",0xA0,3,true},
        {"weapon_guard_chance",0xA4,15,true},{"shield_guard_chance",0xA8,4,true},
        {"dodge_chance",0xAC,5,true},{"critical_chance",0xB0,6,true},{"zone_chance",0xB4,7,true},
        {"strength",0xB8,16,false},{"resilience",0xBC,17,false},{"deftness",0xC0,8,false},
        {"agility",0xC4,9,false},{"magical_might",0xC8,10,false},{"magical_mending",0xCC,11,false},
        {"max_hp",0xD0,12,false},{"max_mp",0xD4,13,false},{"charm",0xD8,14,false}}};
    return bindings;
}
// Write only numeric stat fields of an unpublished private vanilla-row copy.
// Native percentage fields use percentage points, not fractions (3 means +3pp).
// Validate the whole input before writing, so an invalid item cannot partly patch a row.
inline void write_equipment_stats(uint8_t* row,size_t size,const Json& stats) {
    require(row && size>=0xDC && stats.is_object(),"Invalid equipment row/stat object");
    for(auto it=stats.begin();it!=stats.end();++it) {
        require(supported_stat(it.key()) && it.value().is_number(),"Unsupported equipment stat");
        const double value=it.value().get<double>();
        require(std::isfinite(value) && value>=0 && value<=100000,"Invalid equipment stat value");
        require(percentage_stat(it.key()) || std::floor(value)==value,"Point equipment stat must be integral");
    }
    for(const auto& binding:equipment_stat_bindings()) {
        const double value=stats.value(binding.stat,0.0);
        if(binding.percentage) {const float native=static_cast<float>(value);std::memcpy(row+binding.offset,&native,4);}
        else {const int32_t native=static_cast<int32_t>(value);std::memcpy(row+binding.offset,&native,4);}
    }
}
inline void clamp_equipped_chances(uint8_t* parameters,size_t size,const Json& caps) {
    require(parameters && size>=0x4C,"Invalid equipped parameter buffer");
    const std::array<std::pair<const char*,size_t>,4> fields{{
        {"critical_chance",0x40},{"dodge_chance",0x3C},{"weapon_guard_chance",0x38},{"shield_guard_chance",0x34}}};
    std::array<float,4> values{},limits{};
    for(size_t i=0;i<fields.size();++i) {
        std::memcpy(&values[i],parameters+fields[i].second,4);limits[i]=caps.at(fields[i].first);
        require(std::isfinite(values[i]) && std::isfinite(limits[i]) && limits[i]>=0 && limits[i]<=100,"Invalid equipped chance cap/value");
    }
    for(size_t i=0;i<fields.size();++i) if(values[i]>limits[i]) std::memcpy(parameters+fields[i].second,&limits[i],4);
}
}
