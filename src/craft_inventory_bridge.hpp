// Standalone inventory adapter. Uses JackItemManager's own bag insertion,
// removal and equip operations, without any forge accessor or forge session.
void craft_replace_revision(uintptr_t manager,uint64_t source,uint64_t result) {
    if(source==result) return;
    const auto stacks=veyl_stacks(manager,source);
    if(stacks.size()!=1 || stacks.front().count!=1 || owned_count(manager,result))
        throw std::runtime_error("Craft item ownership changed");
    const auto old=stacks.front();const int region=read<int32_t>(old.payload+0x20);
    if(region<0 || region>16) throw std::runtime_error("Unsupported equipment region");
    const int character=reinterpret_cast<int(*)(int)>(base+0x93C400)(old.bag);
    using InsertFn=int(*)(void*,int,uint64_t,int,bool,bool,int*);
    using EquipFn=void(*)(void*,int,int,int);
    const auto insert=reinterpret_cast<InsertFn>(base+0x7692A0);
    const auto equip=reinterpret_cast<EquipFn>(base+0x76EBC0);
    bool removed=false;
    try {
        reinterpret_cast<VeylRemoveFn>(base+0x7811C0)(reinterpret_cast<void*>(manager),old.bag,old.index,1);
        removed=owned_count(manager,source)==0;
        if(!removed) throw std::runtime_error("Craft source removal refused");
        int index=-1;
        const int remaining=insert(reinterpret_cast<void*>(manager),old.bag,result,1,true,true,&index);
        if(remaining || owned_count(manager,result)!=1) throw std::runtime_error("Craft result insertion refused");
        const auto current=veyl_stacks(manager,result);
        if(current.size()!=1 || current.front().bag!=old.bag) throw std::runtime_error("Craft result bag changed");
        if(region) {
            equip(reinterpret_cast<void*>(manager),character,region,current.front().index);
            if(read<int32_t>(veyl_stacks(manager,result).front().payload+0x20)!=region)
                throw std::runtime_error("Craft equipment restoration refused");
        }
    } catch(...) {
        for(const auto& s:veyl_stacks(manager,result))
            reinterpret_cast<VeylRemoveFn>(base+0x7811C0)(reinterpret_cast<void*>(manager),s.bag,s.index,1);
        if(removed && !owned_count(manager,source)) {
            int index=-1;insert(reinterpret_cast<void*>(manager),old.bag,source,1,true,true,&index);
            if(region && owned_count(manager,source)==1)
                equip(reinterpret_cast<void*>(manager),character,region,veyl_stacks(manager,source).front().index);
        }
        if(owned_count(manager,source)!=1 || owned_count(manager,result))
            throw std::runtime_error("Craft rollback failed; stop crafting");
        throw;
    }
}
Json standalone_veyl_commit(uint64_t source,const itemization::VeylChoice& choice,int quote) {
    const auto manager=find_manager();const auto item=item_service->lookup(runtime_ids.at(source));
    const int cost=veyl_policy->cost(*item_service,item,choice);
    const auto currency=veyl_native_ids.at(choice.currency);const int before=owned_count(manager,currency);
    if(cost!=quote || before<cost || !veyl_policy->eligible(*item_service,item,choice))
        throw std::runtime_error("Veyl selection or balance changed");
    const auto changed=veyl_policy->apply(*item_service,item,choice,runtime_random->below(UINT64_MAX));
    const auto result=changed.at("engine_id")==item.at("engine_id")?source:record_runtime_item(changed);
    if(!runtime_row(reinterpret_cast<void*>(read<uintptr_t>(manager+0x38)),result,nullptr,false))
        throw std::runtime_error("Craft result definition unavailable");
    try {veyl_set_quantity(manager,currency,before-cost);craft_replace_revision(manager,source,result);}
    catch(...) {veyl_set_quantity(manager,currency,before);throw;}
    log({{"event","standalone_veyl_committed"},{"currency",choice.currency},{"cost",cost},
        {"source",item.at("engine_id")},{"result",changed.at("engine_id")},{"forge_session",false}});
    return changed;
}
Json standalone_shard_commit(uint64_t source,const std::string& family) {
    const auto manager=find_manager();const auto item=item_service->lookup(runtime_ids.at(source));
    const auto currency=shard_native_ids.at(family);const int before=owned_count(manager,currency);
    if(before<1 || !camp_policy->eligible(*item_service,item,family)) throw std::runtime_error("Shard selection or balance changed");
    const auto changed=camp_policy->apply(*item_service,item,family,runtime_random->below(UINT64_MAX));
    const auto result=record_runtime_item(changed);
    if(!runtime_row(reinterpret_cast<void*>(read<uintptr_t>(manager+0x38)),result,nullptr,false))
        throw std::runtime_error("Shard result definition unavailable");
    try {veyl_set_quantity(manager,currency,before-1);craft_replace_revision(manager,source,result);}
    catch(...) {veyl_set_quantity(manager,currency,before);throw;}
    log({{"event","standalone_shard_committed"},{"family",family},{"source",item.at("engine_id")},
        {"result",changed.at("engine_id")},{"forge_session",false}});return changed;
}
