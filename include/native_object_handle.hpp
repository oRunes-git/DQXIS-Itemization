#pragma once
#include <cstdint>
namespace itemization {
// XI S marks permanently registered class objects in InternalIndex's high bit.
// Bounds and slot/address/serial validation still apply after removing it.
constexpr int native_object_slot(uint32_t index) {return index==0xffffffffU?-1:static_cast<int>(index&0x7fffffffU);}
// A cached UObject must match its original array slot/serial and stay live.
// The caller checks the array bounds before reading the current slot.
struct NativeObjectHandle {
    int index=-1,serial=0;
    uintptr_t address=0;
    bool matches(uintptr_t current,int current_serial,uint32_t internal_flags,uint32_t object_flags) const {
        return index>=0 && address!=0 && current==address && current_serial==serial &&
            !(internal_flags&((1U<<28)|(1U<<29))) && !(object_flags&(0x10U|0x20U|0x8000U|0x10000U));
    }
};
}
