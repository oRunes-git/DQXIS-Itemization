// Stack-scoped state; no retained game object pointers or resource writes.
bool added_fire_prototype_enabled=false;
std::atomic<uint64_t> added_element_log_count{0};
void validate_added_fire_bindings() {
    const std::pair<uintptr_t,std::vector<uint8_t>> bindings[]={
        {0x43AAB0,{0x0F,0xB6,0xC2,0x0F,0xB6,0x04,0x08,0xC3}},
        {0x3DAF07,{0xE8,0x14,0x10,0x00,0x00}}};
    for(const auto& entry:bindings) for(size_t i=0;i<entry.second.size();++i)
        if(read<uint8_t>(base+entry.first+i)!=entry.second[i]) throw std::runtime_error("Added Fire native contract mismatch");
}
struct AddedElementFrame {
    AddedElementFrame* previous=nullptr;
    void* attacker=nullptr;void* target=nullptr;
    std::string action,weapon,damage_row;float raw=0,base_amount=0,resolved=0,merged=0,factor=100;
    bool injected=false;int limit=0;
};
thread_local AddedElementFrame* active_added_element=nullptr;
struct AddedElementScope {
    AddedElementFrame frame;
    AddedElementScope(void* attacker,void* target,void* action,void* context) {
        frame.previous=active_added_element;active_added_element=&frame;
        if(!runtime_enabled.load() || !added_fire_prototype_enabled || !attacker || !target || !action || !context) return;
        try {
            frame.action=fname(read<uint64_t>(reinterpret_cast<uintptr_t>(action)));
            const uintptr_t param=reinterpret_cast<uintptr_t>(context);
            const auto override_id=fname(read<uint64_t>(param+0x20));
            const auto tags=itemization::resolve_native_action_tags(native_scalar_actions,native_scalar_damage_rows,frame.action,override_id);
            if(!tags.is_object()) return;
            frame.damage_row=tags.at("damage_row");
            // The native flag getter at 0x43AAB0 is a byte read. Flag 0xC is
            // copied verbatim to result.bCritical; 0xD controls the metal route.
            if(!itemization::native_fire_prototype_eligible(frame.action,tags,read<uint8_t>(param+0xC)!=0,
                read<uint8_t>(param+0xD)!=0,1,read<int32_t>(param+0x28),read<int32_t>(param+0x2C),false)) return;
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            const auto items=read<Array>(reinterpret_cast<uintptr_t>(attacker)+0x5D8);
            if(items.count<0 || items.count>32 || items.capacity<items.count) return;
            int weapon_count=0;std::vector<Json> equipment;
            for(int i=0;i<items.count;++i) {
                const auto slot=items.data+uintptr_t(i)*16;const auto id=read<uint64_t>(slot);const auto region=read<uint8_t>(slot+8);
                // A populated off hand is conservatively excluded too, even if
                // it is a shield. This first proof uses a two-handed greatsword.
                if((region==1 || region==2) && fname(id)!="None") ++weapon_count;
                const auto found=runtime_ids.find(id);if(found==runtime_ids.end()) continue;
                const auto item=item_service->lookup(found->second);
                if(itemization::native_fire_weapon_eligible(item.at("category"),item.at("base_snapshot").value("equipment_type",std::string{}),region)) {
                    frame.weapon=found->second;equipment.push_back(item);
                }
            }
            if(weapon_count!=1 || equipment.size()!=1) return;
            frame.raw=static_cast<float>(itemization::CombatAffixes::collect(equipment,frame.weapon).get("added_fire"));
            if(!std::isfinite(frame.raw) || frame.raw<0 || frame.raw>100000) throw std::runtime_error("Invalid added Fire roll");
            frame.limit=tags.value("damage_limit",0);if(frame.limit<1 || frame.limit>100000) {frame.raw=0;return;}
            frame.attacker=attacker;frame.target=target;
        } catch(const std::exception& error) {
            frame.raw=0;if(added_element_log_count.fetch_add(1)<250) log({{"event","added_element_failed"},{"reason",error.what()}});
        } catch(...) {frame.raw=0;}
    }
    ~AddedElementScope() {active_added_element=frame.previous;}
};
template<class Resolve> void merge_added_fire(void* attacker,void* target,float* native_output,uintptr_t caller,Resolve resolve) {
    auto* frame=active_added_element;
    if(!frame || frame->raw==0 || frame->injected || attacker!=frame->attacker || target!=frame->target || caller!=base+0x3DAF0C) return;
    try {
        // The audited element helper only consults action-damage byte 0x45.
        // A private row enables that stage, without altering a native data row.
        alignas(8) std::array<uint8_t,0x58> element_row{};element_row[0x45]=1;
        float resolved=frame->raw,factor=100;uint8_t element=0;
        const bool applied=resolve(&resolved,&element,&factor,frame->raw,attacker,target,element_row.data(),1,1);
        if(!applied || element!=1 || !std::isfinite(factor) || factor<0 || factor>1000)
            throw std::runtime_error("Native Fire resistance stage did not resolve");
        const float before=read<float>(reinterpret_cast<uintptr_t>(native_output));
        if(before<0) return; // Separate negative-base component handling is pending.
        const float merged=itemization::native_element_merge(before,resolved);
        SIZE_T written=0;
        if(!WriteProcessMemory(GetCurrentProcess(),native_output,&merged,sizeof(merged),&written) || written!=sizeof(merged))
            throw std::runtime_error("Cannot merge native Fire component");
        frame->base_amount=before;frame->resolved=resolved;frame->merged=merged;frame->factor=factor;frame->injected=true;
    }catch(const std::exception& error) {if(added_element_log_count.fetch_add(1)<250) log({{"event","added_element_failed"},{"reason",error.what()}});}
}
Json finish_added_fire(const AddedElementFrame& frame,void* output) {
    if(!frame.injected) return Json();
    const uintptr_t result=reinterpret_cast<uintptr_t>(output);
    bool guarded=false;for(const size_t offset:{0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x51,0x52,0x53}) guarded|=read<uint8_t>(result+offset)!=0;
    // Native ordinary misses/complete guards must remain zero even if an
    // upstream branch reached the element stage with a zero component.
    if(guarded && read<int32_t>(result+0x1C)!=0) {
        SIZE_T written=0;const int32_t zero=0;
        if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(result+0x1C),&zero,sizeof(zero),&written) || written!=sizeof(zero))
            throw std::runtime_error("Cannot preserve guarded native hit");
    }
    Json details={{"element","fire"},{"raw",frame.raw},{"resistance_percent",frame.factor},{"resolved",frame.resolved},
        {"native_stage_amount",frame.base_amount},{"merged_stage_amount",frame.merged},{"final_damage",read<int32_t>(result+0x1C)},
        {"weapon",frame.weapon},{"damage_row",frame.damage_row},{"guarded",guarded},{"critical",read<uint8_t>(result+0x40)!=0},{"limit",frame.limit},
        {"merge_stage","before native reductions and rounding"},{"additional_attack",false},{"resources_written",false}};
    if(details.at("critical").get<bool>()) throw std::runtime_error("Critical reached noncritical Fire prototype");
    if(added_element_log_count.fetch_add(1)<250) log({{"event","native_added_element_result"},{"action",frame.action},{"component",details}});
    return details;
}
