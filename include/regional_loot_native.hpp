#pragma once
#include <cstdint>
#include <stdexcept>

namespace itemization {
// GetMonsterID returns an FName through the caller's output buffer. Its machine
// return value is that buffer's address, not the FName's bits.
using MonsterNameGetter = uint64_t*(*)(void*,uint64_t*);
inline uint64_t defeated_monster_name(MonsterNameGetter getter,void* character) {
    uint64_t name=0;
    if(getter(character,&name)!=&name) throw std::runtime_error("Native monster-name output mismatch");
    return name;
}
}
