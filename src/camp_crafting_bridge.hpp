// Native game-thread transactions. Currencies live in the adventure log; item
// history only resolves immutable revisions. No save-independent shard wallet.
Json shard_wallet(uintptr_t manager) {
    if(!camp_policy || !manager) throw std::runtime_error("Shard inventory unavailable");
    prepare_veyl_inventory(reinterpret_cast<void*>(manager));Json wallet=Json::object();
    std::map<uint64_t,std::string> shard_names;for(const auto& s:shard_native_ids) {wallet[s.first]=0;shard_names.emplace(s.second,s.first);}
    for(int bag=0;bag<=12;++bag) {
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0 || list.count>10000 || list.capacity<list.count || (list.count && !list.data)) throw std::runtime_error("Invalid shard inventory array");
        for(int i=0;i<list.count;++i) {
            const auto p=read<uintptr_t>(list.data+uintptr_t(i)*16);if(!p)continue;
            const auto found=shard_names.find(read<uint64_t>(p+0x10));if(found==shard_names.end())continue;
            const auto count=read<int32_t>(p+0x18);if(count<1 || count>9999) throw std::runtime_error("Invalid shard quantity");
            wallet[found->second]=wallet.at(found->second).get<int>()+count;
        }
    }
    return wallet;
}
bool salvage_source_valid(uintptr_t manager,uint64_t id) {
    if(!manager || !camp_policy || !runtime_ids.count(id)) return false;
    const auto item=item_service->lookup(runtime_ids.at(id));
    if(camp_policy->protected_item(*item_service,item)) return false;
    const auto stacks=veyl_stacks(manager,id);
    // Native ReplaceRetryKaziItem reads payload+0x20 as EJackEquipmentRegion.
    // Unknown=0 means unequipped; every hand/head/body/accessory region is >0.
    return stacks.size()==1 && itemization::salvage_bag_source(stacks.front().bag,stacks.front().count,read<uint8_t>(stacks.front().payload+0x20));
}
std::vector<Json> salvage_inventory(uintptr_t manager) {
    if(!manager) throw std::runtime_error("Salvage inventory unavailable");
    std::vector<Json> result;std::set<uint64_t> seen;
    for(int bag=itemization::salvage_equipment_bag;bag==itemization::salvage_equipment_bag;++bag) {
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0 || list.count>10000 || list.capacity<list.count || (list.count && !list.data)) throw std::runtime_error("Invalid salvage inventory array");
        for(int i=0;i<list.count;++i) {
            const auto payload=read<uintptr_t>(list.data+uintptr_t(i)*16);if(!payload) continue;
            const auto id=read<uint64_t>(payload+0x10);
            if(!seen.insert(id).second || !runtime_ids.count(id) || read<int32_t>(payload+0x18)!=1 || read<uint8_t>(payload+0x20)!=0) continue;
            auto item=item_service->lookup(runtime_ids.at(id));item["ui_bag"]=bag;
            if(!camp_policy->protected_item(*item_service,item)) result.push_back(std::move(item));
        }
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b) {const auto na=itemization::instance_name(a),nb=itemization::instance_name(b);return na==nb?a.at("engine_id")<b.at("engine_id"):na<nb;});
    return result;
}
Json shard_commit(uintptr_t accessor,uint64_t source,const std::string& family) {
    if(!camp_policy || !runtime_ids.count(source)) throw std::runtime_error("Shard crafting unavailable");
    const auto manager=find_manager();if(!manager) throw std::runtime_error("Item manager unavailable");
    const auto currency=shard_native_ids.at(family);const int before=owned_count(manager,currency);
    if(before<1 || read<uint64_t>(accessor+0x430)!=source) throw std::runtime_error("Shard selection or balance changed");
    veyl_select_current(accessor,manager,source);const auto previous=item_service->lookup(runtime_ids.at(source));
    const auto changed=camp_policy->apply(*item_service,previous,family,runtime_random->below(UINT64_MAX));
    const auto result=record_runtime_item(changed);const auto table=read<uintptr_t>(manager+0x38);
    if(!runtime_row(reinterpret_cast<void*>(table),result,nullptr,false) || owned_count(manager,result)!=0) throw std::runtime_error("Shard result invalid");
    try {
        veyl_set_quantity(manager,currency,before-1);veyl_select_current(accessor,manager,source);
        const auto old_auto=read<uint8_t>(accessor+0x421);*reinterpret_cast<uint8_t*>(accessor+0x421)=0;
        original_forge_replace(reinterpret_cast<void*>(accessor),result);*reinterpret_cast<uint8_t*>(accessor+0x421)=old_auto;
        if(owned_count(manager,source)!=0 || owned_count(manager,result)!=1) throw std::runtime_error("Shard replacement failed");
    } catch(...) {
        if(owned_count(manager,result)==1 && owned_count(manager,source)==0) {
            veyl_select_current(accessor,manager,result);original_forge_replace(reinterpret_cast<void*>(accessor),source);
        }
        veyl_set_quantity(manager,currency,before);throw;
    }
    forge_sessions.erase(accessor);forge_plans.erase(accessor);
    log({{"event","shard_craft_committed"},{"family",family},{"cost",1},{"old_engine_id",previous.at("engine_id")},
        {"new_engine_id",changed.at("engine_id")},{"quality_bp",changed.value("quality_bp",0)},{"affixes",changed.at("affixes")}});
    return changed;
}
Json salvage_commit(const std::vector<std::string>& sources) {
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex);
    if(!camp_policy || sources.empty() || sources.size()>static_cast<size_t>(camp_policy->maximum_items())) throw std::runtime_error("Invalid salvage selection");
    const auto manager=find_manager();if(!manager) throw std::runtime_error("Salvage inventory unavailable");
    std::set<uint64_t> seen;std::vector<uint64_t> native,removed;std::vector<std::string> skipped;
    for(const auto& name:sources) {
        const auto id=native_name(name);
        if(!seen.insert(id).second) throw std::runtime_error("Duplicate salvage selection");
        if(runtime_ids.count(id) && item_service->is_locked(runtime_ids.at(id))) {skipped.push_back(name);continue;}
        if(!salvage_source_valid(manager,id)) throw std::runtime_error("Salvage selection changed or contains equipped/protected gear");
        native.push_back(id);
    }
    Json drops=Json::object();const auto before=shard_wallet(manager);
    try {
        for(const auto id:native) {
            // Re-resolve after every removal because native arrays compact.
            if(item_service->is_locked(runtime_ids.at(id))) {skipped.push_back(runtime_ids.at(id));continue;}
            if(!salvage_source_valid(manager,id)) throw std::runtime_error("Salvage ownership changed");
            const auto item_drops=camp_policy->salvage_roll(*item_service,{item_service->lookup(runtime_ids.at(id))},*runtime_random);
            const auto s=veyl_stacks(manager,id).front();
            reinterpret_cast<VeylRemoveFn>(base+0x7811C0)(reinterpret_cast<void*>(manager),s.bag,s.index,1);
            if(owned_count(manager,id)==0)removed.push_back(id);
            if(owned_count(manager,id)!=0) throw std::runtime_error("Native salvage removal failed");
            for(auto q=item_drops.begin();q!=item_drops.end();++q)drops[q.key()]=drops.value(q.key(),0)+q.value().get<int>();
        }
        for(auto q=drops.begin();q!=drops.end();++q) veyl_set_quantity(manager,shard_native_ids.at(q.key()),
            std::min(9999,before.at(q.key()).get<int>()+q.value().get<int>()));
    } catch(...) {
        for(auto q=before.begin();q!=before.end();++q) veyl_set_quantity(manager,shard_native_ids.at(q.key()),q.value());
        for(const auto id:removed) if(owned_count(manager,id)==0) {uint8_t bag=255;original_store(reinterpret_cast<void*>(manager),id,1,false,&bag);}
        for(const auto id:removed) if(owned_count(manager,id)!=1) throw std::runtime_error("Salvage rollback failed");
        throw;
    }
    Json gained=Json::object();for(auto q=drops.begin();q!=drops.end();++q) gained[q.key()]=owned_count(manager,shard_native_ids.at(q.key()))-before.at(q.key()).get<int>();
    log({{"event","gear_salvaged"},{"items",sources},{"consumed",removed.size()},{"skipped_locked",skipped},{"rolled_shards",drops},{"granted_shards",gained}});
    return {{"salvaged",removed.size()},{"skipped_locked",skipped.size()},{"shards",gained}};
}
