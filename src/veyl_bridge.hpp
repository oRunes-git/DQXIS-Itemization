// Included after the forge's native replacement binding. All inventory mutations
// occur on the game's UI/reward callback, under the shared recursive item lock.
using VeylRemoveFn=int(*)(void*,int,int,int);
uintptr_t loot_save(void* context);
bool veyl_claimed(uintptr_t save,const std::string& flag);
void veyl_claim(uintptr_t save,const std::string& flag);
void remember_kingmaker_acquisition(uintptr_t manager,uint64_t item,int accepted) {
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    if(!veyl_policy || accepted<=0 || !manager)return;
    const auto found=veyl_native_ids.find("kingmaker");
    if(found==veyl_native_ids.end() || found->second!=item)return;
    const auto save=loot_save(reinterpret_cast<void*>(manager));
    if(!save)throw std::runtime_error("Kingmaker acquisition save context unavailable");
    const std::string flag=veyl_policy->definition("kingmaker").at("acquisition_flag");
    if(!veyl_claimed(save,flag))veyl_claim(save,flag);
}
void validate_veyl_bindings() {
    const std::vector<std::pair<uintptr_t,std::vector<uint8_t>>> guards{
        {0xD68EE0,{0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x57}},
        {0x7811C0,{0x44,0x89,0x4C,0x24,0x20,0x55,0x57}},
        {0x759EA0,{0x48,0x89,0x5C,0x24,0x08}},
        {0x92AF40,{0x48,0x89,0x54,0x24,0x10}},
        {0x930390,{0x48,0x89,0x5C,0x24,0x08}},
        {0x1BDB340,{0x33,0xD2,0xE9,0xB9,0x62,0x0E,0xFF}},
        {0x92CF80,{0x0F,0xB6,0x4E,0x20}}}; // Native equipped-region payload field.
    for(const auto& g:guards) for(size_t i=0;i<g.second.size();++i)
        if(read<uint8_t>(base+g.first+i)!=g.second[i]) throw std::runtime_error("Veyl inventory/claim native binding mismatch");
}
void feedback_require_property(uintptr_t cls,const std::string& property,int offset);
void prepare_veyl_inventory(void* raw) {
    if(!veyl_policy || !raw) return;
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    const auto manager=reinterpret_cast<uintptr_t>(raw);const auto handle=manager_handle(manager);
    static std::set<std::pair<int,int>> prepared;if(prepared.count({handle.index,handle.serial}))return;
    const auto table=read<uintptr_t>(manager+0x38);
    if(!table) throw std::runtime_error("Native currency item table unavailable");
    for(const auto& c:veyl_native_ids) if(!original_item_row(reinterpret_cast<void*>(table),c.second,nullptr,false))
        throw std::runtime_error("Native Veyl row unavailable: "+c.first);
    for(const auto& c:shard_native_ids) if(!original_item_row(reinterpret_cast<void*>(table),c.second,nullptr,false))
        throw std::runtime_error("Native shard row unavailable: "+c.first);
    feedback_require_property(read<uintptr_t>(manager+0x10),"ItemInnerCountMax_Exchange_Individual",0x70);
    const auto before=read<Array>(manager+0x70);
    if(before.count<0 || before.count>1000 || before.capacity<before.count || (before.count && !before.data))
        throw std::runtime_error("Invalid native currency maximum table");
    // FJackItemInnerCountMax is a value-only FName + int (16 bytes). Copy the
    // instance's own array with the game's allocator, retaining every vanilla
    // entry and adding the currency/shard identities. No CDO is touched.
    struct Limit {uint64_t item;int32_t maximum,padding;};static_assert(sizeof(Limit)==16,"Native stack limit ABI");
    std::vector<Limit> limits;std::set<uint64_t> seen;
    for(int i=0;i<before.count;++i) {const auto entry=read<Limit>(before.data+uintptr_t(i)*16);limits.push_back(entry);seen.insert(entry.item);}
    for(const auto& c:veyl_native_ids) if(seen.insert(c.second).second) limits.push_back({c.second,9999,0});
    for(const auto& c:shard_native_ids) if(seen.insert(c.second).second) limits.push_back({c.second,9999,0});
    if(limits.size()!=static_cast<size_t>(before.count)) {
        const auto bytes=limits.size()*sizeof(Limit);
        const auto memory=reinterpret_cast<uintptr_t(*)(uint64_t)>(base+0x1BDB340)(bytes);
        if(!memory) throw std::runtime_error("Native currency limit allocation failed");
        std::memcpy(reinterpret_cast<void*>(memory),limits.data(),bytes);
        *reinterpret_cast<Array*>(manager+0x70)=Array{memory,static_cast<int32_t>(limits.size()),static_cast<int32_t>(limits.size())};
        if(before.data) reinterpret_cast<void(*)(void*)>(base+0xCBDFB0)(reinterpret_cast<void*>(before.data));
    }
    prepared.insert({handle.index,handle.serial});
}
struct VeylStack {int bag,index,count;uintptr_t payload;};
std::vector<VeylStack> veyl_stacks(uintptr_t manager,uint64_t id) {
    std::vector<VeylStack> out;
    for(int bag=0;bag<=12;++bag) {
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0 || list.count>10000 || list.capacity<list.count || (list.count && !list.data))
            throw std::runtime_error("Invalid native inventory stack array");
        for(int i=0;i<list.count;++i) {
            const auto p=read<uintptr_t>(list.data+uintptr_t(i)*16);
            if(p && read<uint64_t>(p+0x10)==id) {
                const int n=read<int32_t>(p+0x18);if(n<1 || n>9999) throw std::runtime_error("Invalid native Veyl stack");
                out.push_back({bag,i,n,p});
            }
        }
    }
    return out;
}
Json veyl_wallet(uintptr_t manager) {
    if(!manager) throw std::runtime_error("Native item manager unavailable");
    prepare_veyl_inventory(reinterpret_cast<void*>(manager));
    Json wallet=Json::object();std::map<uint64_t,std::string> currency_names;
    for(const auto& c:veyl_native_ids) {wallet[c.first]=0;currency_names.emplace(c.second,c.first);}
    for(int bag=0;bag<=12;++bag) {
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0 || list.count>10000 || list.capacity<list.count || (list.count && !list.data))
            throw std::runtime_error("Invalid currency inventory array");
        for(int i=0;i<list.count;++i) {
            const auto p=read<uintptr_t>(list.data+uintptr_t(i)*16);if(!p)continue;
            const auto name=currency_names.find(read<uint64_t>(p+0x10));if(name==currency_names.end())continue;
            const int n=read<int32_t>(p+0x18);if(n<1 || n>9999) throw std::runtime_error("Invalid currency stack quantity");
            wallet[name->second]=wallet.at(name->second).get<int>()+n;
        }
    }
    const auto save=loot_save(reinterpret_cast<void*>(manager));
    if(!save)throw std::runtime_error("Currency discovery save context unavailable");
    const std::string flag=veyl_policy->definition("kingmaker").at("acquisition_flag");
    bool discovered=veyl_claimed(save,flag);
    if(wallet.value("kingmaker",0)>0 && !discovered) {
        veyl_claim(save,flag);discovered=true;
    }
    wallet["kingmaker_acquired"]=discovered;return wallet;
}
void veyl_set_quantity(uintptr_t manager,uint64_t id,int desired) {
    prepare_veyl_inventory(reinterpret_cast<void*>(manager));
    const int before=owned_count(manager,id);
    if(desired<0 || desired>9999) throw std::runtime_error("Veyl quantity outside native stack limit");
    if(desired>before) {
        uint8_t bag=255;const int remaining=original_store(reinterpret_cast<void*>(manager),id,desired-before,false,&bag);
        if(remaining!=0 || owned_count(manager,id)!=desired) {
            log({{"event","native_currency_credit_refused"},{"item",fname(id)},{"before",before},{"desired",desired},
                {"actual",owned_count(manager,id)},{"remaining",remaining},{"bag",bag}});
            throw std::runtime_error("Shard/currency grant refused: "+fname(id));
        }
        remember_kingmaker_acquisition(manager,id,desired-before);
    } else while(owned_count(manager,id)>desired) {
        const auto stacks=veyl_stacks(manager,id);if(stacks.empty()) throw std::runtime_error("Veyl debit stack missing");
        const auto s=stacks.back();const int n=std::min(s.count,owned_count(manager,id)-desired);
        const int previous=owned_count(manager,id);
        reinterpret_cast<VeylRemoveFn>(base+0x7811C0)(reinterpret_cast<void*>(manager),s.bag,s.index,n);
        if(owned_count(manager,id)!=previous-n) throw std::runtime_error("Native currency debit refused");
    }
    if(owned_count(manager,id)!=desired) throw std::runtime_error("Native currency balance verification failed");
}
// Prepare the audited native replacement routine's selector from a fresh bag
// scan, rather than retaining an index after another item was inserted/removed.
void veyl_select_current(uintptr_t accessor,uintptr_t manager,uint64_t id) {
    const auto stacks=veyl_stacks(manager,id);
    if(stacks.size()!=1 || stacks.front().count!=1) throw std::runtime_error("Craft source is not uniquely owned");
    const auto detail=read<uintptr_t>(accessor+0x440);
    if(!detail) throw std::runtime_error("Native craft detail unavailable");
    *reinterpret_cast<int32_t*>(accessor+0x42C)=stacks.front().bag;
    *reinterpret_cast<uint64_t*>(accessor+0x430)=id;
    *reinterpret_cast<int32_t*>(accessor+0x438)=stacks.front().index;
    *reinterpret_cast<int32_t*>(detail+0x70)=stacks.front().index;
}
Json veyl_commit(uintptr_t accessor,uint64_t source,const itemization::VeylChoice& choice,int quoted_cost) {
    if(!veyl_policy || !runtime_ids.count(source)) throw std::runtime_error("Veyl crafting unavailable");
    const auto manager=find_manager();const Json item=item_service->lookup(runtime_ids.at(source));
    const int cost=veyl_policy->cost(*item_service,item,choice);const uint64_t currency=veyl_native_ids.at(choice.currency);
    const int before=owned_count(manager,currency);
    if(!manager || cost!=quoted_cost || before<cost || read<uint64_t>(accessor+0x430)!=source)
        throw std::runtime_error("Craft selection or currency balance changed");
    veyl_select_current(accessor,manager,source);
    const auto changed=veyl_policy->apply(*item_service,item,choice,runtime_random->below(UINT64_MAX));
    const auto result=changed.at("engine_id")==item.at("engine_id")?source:record_runtime_item(changed);
    if(result!=source && owned_count(manager,result)!=0) throw std::runtime_error("Craft result already owned");
    const auto table=read<uintptr_t>(manager+0x38);
    if(!runtime_row(reinterpret_cast<void*>(table),result,nullptr,false)) throw std::runtime_error("Craft result base unavailable");
    bool replaced=false;
    try {
        // Debit before replacing; any refused replacement refunds that exact
        // native stack. History revisions remain immutable even after rollback.
        veyl_set_quantity(manager,currency,before-cost);
        veyl_select_current(accessor,manager,source);
        if(result!=source) {
            const auto previous_auto_equip=read<uint8_t>(accessor+0x421);
            *reinterpret_cast<uint8_t*>(accessor+0x421)=0;
            original_forge_replace(reinterpret_cast<void*>(accessor),result);
            *reinterpret_cast<uint8_t*>(accessor+0x421)=previous_auto_equip;
            replaced=owned_count(manager,result)==1 && owned_count(manager,source)==0;
            if(!replaced) throw std::runtime_error("Native equipment replacement failed");
        }
    } catch(...) {
        if(replaced || (result!=source && owned_count(manager,result)==1 && owned_count(manager,source)==0)) {
            veyl_select_current(accessor,manager,result);original_forge_replace(reinterpret_cast<void*>(accessor),source);
            if(owned_count(manager,source)!=1 || owned_count(manager,result)!=0) throw std::runtime_error("Native equipment rollback failed");
        }
        veyl_set_quantity(manager,currency,before);throw;
    }
    forge_plans.erase(accessor);forge_sessions.erase(accessor);
    log({{"event","veyl_craft_committed"},{"currency",choice.currency},{"cost",cost},{"target",choice.target},
        {"old_engine_id",item.at("engine_id")},{"new_engine_id",changed.at("engine_id")},
        {"instance_id",changed.at("instance_id")},{"affixes",changed.at("affixes")},{"wallet",veyl_wallet(manager)}});
    return changed;
}
Json veyl_recipe_commit(uintptr_t accessor,uint64_t recipe) {
    if(!veyl_policy || read<uint64_t>(accessor+0x220)!=recipe) throw std::runtime_error("Selected recipe changed");
    const auto group=read<Array>(accessor+0x228);
    if(group.count<1 || group.count>16 || group.capacity<group.count || !group.data) throw std::runtime_error("Native recipe result unavailable");
    const auto id=fname(read<uint64_t>(group.data));const auto b=runtime_bases.find(id);
    if(b==runtime_bases.end() || !b->second.value("automatic_acquisition",true)) throw std::runtime_error("Recipe base is protected or unsupported");
    const auto manager=find_manager();if(!manager) throw std::runtime_error("Native recipe inventory unavailable");
    const auto materials=read<Array>(accessor+0x1D8);
    if(materials.count<1 || materials.count>5 || materials.capacity<materials.count || !materials.data) throw std::runtime_error("Recipe materials unavailable");
    if(!reinterpret_cast<bool(*)(void*,uint64_t)>(base+0x92AF40)(reinterpret_cast<void*>(accessor),recipe))
        throw std::runtime_error("Required recipe materials are missing");
    const auto item=item_service->generate(id,"normal",runtime_random->below(UINT64_MAX),new_logical_id());
    const auto result=record_runtime_item(item);
    const auto table=read<uintptr_t>(manager+0x38);
    if(!runtime_row(reinterpret_cast<void*>(table),result,nullptr,false) || owned_count(manager,result)!=0)
        throw std::runtime_error("Recipe result failed validation");
    // ReflectResultKaziItem is the native finished-craft commit: it consumes the
    // bound materials and grants one item. Do not separately subtract materials.
    original_forge_reflect(reinterpret_cast<void*>(accessor),result,false);
    if(owned_count(manager,result)!=1) throw std::runtime_error("Native instant recipe did not grant its result");
    reinterpret_cast<bool(*)(void*,uint64_t,int,bool)>(base+0x930390)(reinterpret_cast<void*>(accessor),recipe,0,false);
    log({{"event","veyl_instant_recipe"},{"recipe",fname(recipe)},{"engine_id",item.at("engine_id")},{"rarity","normal"},{"minigame",false},{"pearls_spent",0}});
    return item;
}
