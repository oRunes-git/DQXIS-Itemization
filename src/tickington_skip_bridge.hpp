// Only native checkpoint flag access and the Hotto-completion write are adapted.
using TickingtonFlagGetFn=bool(*)(void*,int,uint64_t);
using TickingtonFlagSetFn=void(*)(void*,int,uint64_t,bool);
TickingtonFlagGetFn original_tickington_flag_get=nullptr;
TickingtonFlagSetFn original_tickington_flag_set=nullptr;
uint64_t tickington_first_return_id=0,tickington_hotto_complete_id=0;

void tickington_apply_skip(void* context) {
    if(!context || !reinterpret_cast<SaveGetterFn>(base+0x7DCF70)(context)) return;
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    const bool changed=itemization::apply_tickington_skip([&](int category,const char* id) {
        const auto native_id=id==itemization::tickington_first_return?tickington_first_return_id:tickington_hotto_complete_id;
        return original_tickington_flag_get(context,category,native_id);
    },[&](int category,const char*,bool value) {
        original_tickington_flag_set(context,category,tickington_first_return_id,value);
    });
    if(changed) log({{"event","tickington_introduction_skipped"},{"condition","Hotto completed"},
        {"flag",itemization::tickington_first_return},{"flag_category",4},{"inventory_changed",false}});
}
void tickington_skip_report(const std::exception& e) {
    static std::set<std::string> reported;
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    if(reported.insert(e.what()).second) log({{"event","tickington_skip_refused"},{"reason",e.what()}});
}
bool tickington_flag_get_hook(void* context,int category,uint64_t id) {
    if(runtime_enabled.load() && category==itemization::tickington_system_category && id==tickington_first_return_id)
        try {tickington_apply_skip(context);}catch(const std::exception& e){tickington_skip_report(e);}
    return original_tickington_flag_get(context,category,id);
}
void tickington_flag_set_hook(void* context,int category,uint64_t id,bool value) {
    original_tickington_flag_set(context,category,id,value);
    if(runtime_enabled.load() && value && category==itemization::tickington_scenario_category && id==tickington_hotto_complete_id)
        try {tickington_apply_skip(context);}catch(const std::exception& e){tickington_skip_report(e);}
}
void create_tickington_skip_hooks() {
    const std::vector<std::pair<uintptr_t,std::vector<uint8_t>>> guards{
        {0x751430,{0x4C,0x89,0x44,0x24,0x18}},
        {0x759EA0,{0x48,0x89,0x5C,0x24,0x08}}};
    for(const auto& g:guards)for(size_t i=0;i<g.second.size();++i)
        if(read<uint8_t>(base+g.first+i)!=g.second[i]) throw std::runtime_error("Tickington skip native binding mismatch");
    tickington_first_return_id=native_name(itemization::tickington_first_return);
    tickington_hotto_complete_id=loot_native_wide_name(itemization::tickington_hotto_complete);
    if(fname(tickington_hotto_complete_id)!=itemization::tickington_hotto_complete)
        throw std::runtime_error("Hotto completion flag name mismatch");
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x751430),reinterpret_cast<void*>(&tickington_flag_get_hook),reinterpret_cast<void**>(&original_tickington_flag_get))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(base+0x759EA0),reinterpret_cast<void*>(&tickington_flag_set_hook),reinterpret_cast<void**>(&original_tickington_flag_set))!=MH_OK ||
       MH_EnableHook(reinterpret_cast<void*>(base+0x751430))!=MH_OK ||
       MH_EnableHook(reinterpret_cast<void*>(base+0x759EA0))!=MH_OK)
        throw std::runtime_error("Tickington introduction skip hook failed");
}
