// Common result boundary: passive tracing and optional pure scalar multipliers.
// Called by real actions and preview/AI paths. Pure multipliers may update both;
// future resource/status side effects need an authoritative application boundary.
using NativeDamageFn=void*(*)(void*,void*,void*,void*,void*);
NativeDamageFn original_damage_calculator=nullptr;
// Element helper ABI audited from the common calculator's nine arguments.
// The fourth argument is a float in XMM3; the five remaining arguments are on
// the stack. Forward all of them once, retaining the original native result.
using NativeElementFn=bool(*)(float*,uint8_t*,float*,float,void*,void*,void*,uint8_t,uint8_t);
NativeElementFn original_element_helper=nullptr;
struct ElementTraceFrame {
    Json stages=Json::array();ElementTraceFrame* previous=nullptr;
};
thread_local ElementTraceFrame* active_element_trace=nullptr;
struct ElementTraceScope {
    ElementTraceFrame frame;
    explicit ElementTraceScope(bool enabled) {frame.previous=active_element_trace;if(enabled) active_element_trace=&frame;}
    ~ElementTraceScope() {active_element_trace=frame.previous;}
};
bool combat_trace_enabled=false;
std::atomic<uint64_t> combat_trace_count{0};
bool element_trace_hook(float* damage,uint8_t* element,float* resistance,float input,void* attacker,void* target,
    void* action_damage,uint8_t handedness,uint8_t explicit_element) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_element_helper(damage,element,resistance,input,attacker,target,action_damage,handedness,explicit_element);
    if(combat_dispatch_enabled) dispatch_capture_element(attacker,target,*element,*resistance,caller);
    if(!active_element_trace && (!active_added_element || active_added_element->raw==0)) return result;
    try {
        const float native_output=read<float>(reinterpret_cast<uintptr_t>(damage));
        merge_added_fire(attacker,target,damage,caller,original_element_helper);
        if(active_element_trace) active_element_trace->stages.push_back({{"input",input},{"output",native_output},
            {"element",read<uint8_t>(reinterpret_cast<uintptr_t>(element))},{"resistance_factor",read<float>(reinterpret_cast<uintptr_t>(resistance))},
            {"explicit_element",explicit_element},{"handedness",handedness},{"applied",result},
            {"caller_rva",caller>=base?caller-base:0}});
    } catch(...) {} // Diagnostic read failures must never alter combat.
    return result;
}
void* damage_trace_hook(void* output,void* attacker,void* target,void* action,void* context) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    ElementTraceScope trace(runtime_enabled.load() && combat_trace_enabled && combat_trace_count.load()<250);
    AddedElementScope added(attacker,target,action,context);
    DispatchScope dispatch(attacker,target,action,context);
    auto result=original_damage_calculator(output,attacker,target,action,context);
    Json element_component;
    try {element_component=finish_added_fire(added.frame,output);}
    catch(const std::exception& error) {if(added_element_log_count.fetch_add(1)<250) log({{"event","added_element_failed"},{"reason",error.what()}});}
    const auto scalar=combat_dispatch_enabled?Json():apply_native_combat_scalar(output,attacker,context);
    try {
        if(!runtime_enabled.load() || !combat_trace_enabled || combat_trace_count.load()>=250) return result;
        const uintptr_t source=reinterpret_cast<uintptr_t>(attacker),destination=reinterpret_cast<uintptr_t>(target),damage=reinterpret_cast<uintptr_t>(output);
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        Json equipped=Json::array();
        if(source && manager_object_is_a(source,"JackFriendGameCharacter")) {
            const auto items=read<Array>(source+0x5D8);
            if(items.count<0 || items.count>32 || items.capacity<items.count) return result;
            for(int i=0;i<items.count;++i) {
                const auto id=read<uint64_t>(items.data+uintptr_t(i)*16);auto found=runtime_ids.find(id);
                if(found!=runtime_ids.end()) equipped.push_back(found->second);
            }
        }
        if(equipped.empty()) return result;
        Json flags=Json::object();
        for(const auto& field:std::array<std::pair<const char*,size_t>,10>{{
            {"critical",0x40},{"dodge",0x44},{"shield_guard",0x45},{"weapon_guard",0x46},
            {"miss",0x47},{"invulnerable",0x48},{"damage_guard",0x49},{"physical_guard",0x4A},{"critical_guard",0x4B},{"no_parameter_change",0x51}}})
            flags[field.first]=read<uint8_t>(damage+field.second)!=0;
        if(combat_trace_count.fetch_add(1)>=250) return result;
        log({{"event","combat_affix_boundary_trace"},{"attacker",fname(read<uint64_t>(source+0x38))},
            {"target",destination?fname(read<uint64_t>(destination+0x38)):"None"},
            {"action",fname(read<uint64_t>(damage))},{"damage",scalar.is_object()?scalar.at("before"):Json(read<int32_t>(damage+0x1C))},
            {"effective_damage",read<int32_t>(damage+0x1C)},{"scalar",scalar},
            {"added_element",element_component},{"element_modified",added.frame.injected},
            {"shared_dispatch",combat_dispatch_enabled},{"dispatch_applied",dispatch.frame.terminal_applied},
            {"hit_coefficient",dispatch.frame.scale.coefficient},{"coefficient_valid",dispatch.frame.scale.valid},
            {"element",read<uint8_t>(damage+0x20)},{"resistance_factor",read<float>(damage+0x24)},
            {"flags",flags},{"generated_equipment",equipped},{"result_pointer_matches",result==output},
            {"caller_rva",caller>=base?caller-base:0},{"native_element_stages",trace.frame.stages},
            {"source_hp",read<int32_t>(source+0x7C)},{"target_hp",destination?Json(read<int32_t>(destination+0x7C)):Json(nullptr)},
            {"target_handle",read<uint64_t>(damage+0x10)},
            {"damage_type",read<uint8_t>(damage+0x18)},
            {"authoritative_application_verified",false},{"damage_modified",scalar.is_object() && scalar.at("before")!=scalar.at("after")}});
    }catch(const std::exception& error) {
        if(combat_trace_count.fetch_add(1)<250) log({{"event","combat_affix_trace_failed"},{"reason",error.what()}});
    }
    return result;
}
void create_combat_trace_hook() {
    const std::pair<uintptr_t,std::array<uint8_t,5>> signature={0x3DA470,{0x48,0x89,0x5C,0x24,0x18}};
    for(size_t i=0;i<signature.second.size();++i) if(read<uint8_t>(base+signature.first+i)!=signature.second[i])
        throw std::runtime_error("Combat calculator signature mismatch");
    if(MH_CreateHook(reinterpret_cast<void*>(base+signature.first),reinterpret_cast<void*>(&damage_trace_hook),reinterpret_cast<void**>(&original_damage_calculator))!=MH_OK)
        throw std::runtime_error("Combat trace hook creation failed");
    const std::pair<uintptr_t,std::array<uint8_t,5>> element_signature={0x3DBF20,{0x40,0x55,0x57,0x41,0x56}};
    for(size_t i=0;i<element_signature.second.size();++i) if(read<uint8_t>(base+element_signature.first+i)!=element_signature.second[i])
        throw std::runtime_error("Element helper signature mismatch");
    if(MH_CreateHook(reinterpret_cast<void*>(base+element_signature.first),reinterpret_cast<void*>(&element_trace_hook),reinterpret_cast<void**>(&original_element_helper))!=MH_OK)
        throw std::runtime_error("Element trace hook creation failed");
    dispatch_element_resolver=original_element_helper;
}
