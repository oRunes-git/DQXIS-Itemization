// First forge adapter: the native minigame/cost/equipment routines stay in use.
// Transient result-group slots reference one resolved instance, not grade variants.
bool forge_modifiers_enabled=false;
std::unique_ptr<itemization::ForgePolicy> forge_policy;
std::unique_ptr<itemization::ForgeNavigation> forge_navigation;
itemization::ForgeNavigation& forge_navigation_cache() {
    if(!forge_navigation) forge_navigation=std::make_unique<itemization::ForgeNavigation>(*item_service,*forge_policy);
    return *forge_navigation;
}
struct ForgeSession {
    bool started=false,retry=false,modded=false,prepared=false,committed=false,paid=false;
    uint64_t old_id=0,result_id=0;
    int outcome=6;
    itemization::ForgeChoice choice;
    int pearl_cost=0;
    bool performance_measured=false;
    itemization::ForgePerformance performance;
};
std::map<uintptr_t,ForgeSession> forge_sessions;
struct ForgePlan {uint64_t source;itemization::ForgeChoice choice;int cost;};
std::map<uintptr_t,ForgePlan> forge_plans;
uintptr_t recent_forge_accessor=0;
bool forge_custom_item(uint64_t id) {
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    return runtime_enabled.load() && forge_modifiers_enabled && runtime_ids.count(id)!=0;
}
thread_local bool forge_read_base_cost=false;
int forge_base_pearl_cost(void* accessor,uint64_t id) {
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    if(forge_custom_item(id)) {
        const auto& item=item_service->lookup(runtime_ids.at(id));
        const auto name=item.at("base_snapshot").at("native_base_id").get<std::string>();
        auto found=native_base_ids.find(name);
        if(found==native_base_ids.end()) found=native_base_ids.emplace(name,native_name(name)).first;
        id=found->second;
    }
    // Native cost resolves the base's recipe and reads its original pearl column.
    // Quality is separate from vanilla +grades, so always use the unenhanced cost.
    const bool previous=forge_read_base_cost;forge_read_base_cost=true;
    int cost;
    try {cost=original_forge_cost(accessor,id);} catch(...) {forge_read_base_cost=previous;throw;}
    forge_read_base_cost=previous;
    if(cost<=0) throw std::runtime_error("Item base has no valid original pearl cost");
    return cost;
}
void forge_publish_result(void* accessor,uint64_t id) {
    auto address=reinterpret_cast<uintptr_t>(accessor);
    auto group=read<Array>(address+0x228);
    if (group.count<=0 || group.count>16 || group.capacity<group.count)
        throw std::runtime_error("Unsupported transient forge result group");
    // SettingRecipeInfo moves a freshly allocated copy into this accessor.
    // These are value-only FNames; no global table or shared group is modified.
    for (int i=0;i<group.count;++i)
        *reinterpret_cast<uint64_t*>(group.data+uintptr_t(i)*8)=id;
}
void forge_mod_start(void* accessor,bool retry) noexcept {
    if (!forge_modifiers_enabled) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        ForgeSession fresh;fresh.started=true;fresh.retry=retry;
        forge_sessions[reinterpret_cast<uintptr_t>(accessor)]=fresh;
    } catch (...) {log({{"event","forge_session_initialization_failed"}});}
}
void forge_mod_select(void* accessor,uint64_t id) noexcept {
    if (!forge_custom_item(id)) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto address=reinterpret_cast<uintptr_t>(accessor);
        auto& session=forge_sessions[address];session.started=true;session.retry=true;
        session.modded=true;session.old_id=id;session.result_id=id;
        const auto item=item_service->lookup(runtime_ids.at(id));
        auto plan=forge_plans.find(address);
        const bool quality=bool(camp_policy);
        if (!quality && (plan==forge_plans.end() || plan->second.source!=id ||
            !forge_policy->can_apply(*item_service,item,plan->second.choice)))
            throw std::runtime_error("Selected item has no validated crafting choice");
        if(quality) {session.choice.operation="quality";session.pearl_cost=forge_base_pearl_cost(accessor,id);}
        else {session.choice=plan->second.choice;session.pearl_cost=plan->second.cost;}
        if (owned_count(find_manager(),id)!=1) throw std::runtime_error("Forge instance must be owned exactly once");
        forge_publish_result(accessor,id);
        log({{"event","forge_instance_selected"},{"engine_id",item.at("engine_id")},
            {"instance_id",item.at("instance_id")},{"affixes",item.at("affixes")},
            {"operation",session.choice.operation},{"target_affix",session.choice.affix_id},
            {"chosen_replacement",session.choice.replacement_id},{"pearl_cost",session.pearl_cost}});
    } catch (const std::exception& error) {log({{"event","forge_select_refused"},{"reason",error.what()}});}
}
void forge_mod_judged(void* accessor,int outcome) noexcept {
    if (!forge_modifiers_enabled) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto address=reinterpret_cast<uintptr_t>(accessor);
        auto found=forge_sessions.find(address);
        if (found==forge_sessions.end() || !found->second.started || found->second.committed) return;
        auto& session=found->second;
        if (session.prepared) return; // The first final judgment resolves one result.
        if (outcome<0 || outcome>4) throw std::runtime_error("Unsupported forge minigame result");
        const auto manager=find_manager();
        Json result;
        if (session.retry) {
            if (!session.modded || !forge_custom_item(session.old_id)) return;
            if (read<uint64_t>(address+0x430)!=session.old_id || owned_count(manager,session.old_id)!=1)
                throw std::runtime_error("Selected forge instance changed before judgment");
            if (!session.paid) throw std::runtime_error("Crafting choice has not completed its pearl charge");
            const auto old=item_service->lookup(runtime_ids.at(session.old_id));
            if(camp_policy && session.choice.operation=="quality") {
                if(!session.performance_measured) throw std::runtime_error("Quality requires measured forging performance");
                result=item_service->reforge_quality(old.at("engine_id"),session.performance.score);
            } else if(forge_policy->uses_performance()) {
                if(!session.performance_measured) throw std::runtime_error("Generated forge result has no validated performance");
                result=forge_policy->apply_performance(*item_service,old,session.choice,session.performance.score,runtime_random->below(UINT64_MAX));
            } else result=forge_policy->apply(*item_service,old,outcome,session.choice,runtime_random->below(UINT64_MAX));
            session.result_id=result.at("engine_id")==old.at("engine_id")?session.old_id:record_runtime_item(result);
        } else {
            auto group=read<Array>(address+0x228);
            if (group.count<=0 || group.count>16 || group.capacity<group.count)
                throw std::runtime_error("Forge creation result group unavailable");
            const auto label=fname(read<uint64_t>(group.data));
            const auto definition=runtime_bases.find(label);
            if (definition==runtime_bases.end() || !definition->second.value("automatic_acquisition",true) ||
                std::stoi(definition->second.at("native_fields").at("LimitedNumber").get<std::string>())>0) {
                log({{"event","forge_kept_vanilla"},{"base_id",label},{"reason","unsupported or story-limited forge base"}});return;
            }
            // The forge's finished acquisition uses the same ItemService generator
            // as rewards and shops. A minigame enhancement uses the same edit core.
            result=item_service->generate(label,"normal",runtime_random->below(UINT64_MAX),new_logical_id());
            if(camp_policy) {
                if(!session.performance_measured) throw std::runtime_error("Recipe quality requires measured forging performance");
                result=item_service->finish_quality(result,session.performance.score);
            } else result=forge_policy->finish_fresh(*item_service,result,outcome,runtime_random->below(UINT64_MAX));
            session.result_id=record_runtime_item(result);session.modded=true;
        }
        auto table=read<uintptr_t>(manager+0x38);
        if (!runtime_row(reinterpret_cast<void*>(table),session.result_id,nullptr,false))
            throw std::runtime_error("Resolved forge row unavailable");
        forge_publish_result(accessor,session.result_id);
        session.prepared=true;session.outcome=outcome;
        log({{"event","forge_instance_prepared"},{"retry",session.retry},{"native_outcome",outcome},
            {"operation",session.retry?Json(session.choice.operation):Json("fresh_craft")},
            {"target_affix",session.choice.affix_id},{"chosen_replacement",session.choice.replacement_id},
            {"old_engine_id",session.retry?Json(fname(session.old_id)):Json(nullptr)},
            {"new_engine_id",result.at("engine_id")},{"instance_id",result.at("instance_id")},
            {"affixes",result.at("affixes")},{"effective_stats",itemization::effective_stats(result)},
            {"owned_before_commit",owned_count(manager,session.result_id)}});
    } catch (const std::exception& error) {
        log({{"event","forge_preparation_refused"},{"reason",error.what()}});
        try {
            auto& session=forge_sessions.at(reinterpret_cast<uintptr_t>(accessor));
            if (session.retry && session.modded) forge_publish_result(accessor,session.old_id);
        } catch (...) {}
    } catch (...) {log({{"event","forge_preparation_failed"}});}
}
// Return true to signal that the generated-item branch was handled. Never fall
// back to a native grade on failure, because it would discard the logical identity.
bool forge_mod_replace(void* accessor,uint64_t requested) noexcept {
    if (!forge_modifiers_enabled) return false;
    bool handled=false;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto address=reinterpret_cast<uintptr_t>(accessor);
        const auto selected=read<uint64_t>(address+0x430);
        if (!forge_custom_item(selected)) return false;
        handled=true;
        auto& session=forge_sessions.at(address);
        if (!session.retry || !session.modded || !session.prepared || session.old_id!=selected)
            throw std::runtime_error("Generated rework has no validated prepared result");
        if (session.committed) return true; // UI repetition cannot reapply an edit.
        if (!itemization::forge_commit_request_matches(requested,session.old_id,session.result_id,session.outcome,session.choice.operation))
            throw std::runtime_error("Forge UI did not select the resolved revision");
        const auto manager=find_manager();
        if (owned_count(manager,selected)!=1) throw std::runtime_error("Rework source must remain uniquely owned");
        if (session.result_id!=selected) {
            if (owned_count(manager,session.result_id)!=0) throw std::runtime_error("Prepared rework revision is already owned");
            // Original routine replaces exactly one bag item and restores equipped
            // and dual-wield associations, using the resolved revision as its result.
            original_forge_replace(accessor,session.result_id);
            if (owned_count(manager,session.result_id)!=1 || owned_count(manager,selected)!=0)
                throw std::runtime_error("Native rework replacement did not preserve single ownership");
        }
        session.committed=true;
        forge_plans.erase(address);
        const auto item=item_service->lookup(runtime_ids.at(session.result_id));
        log({{"event","forge_instance_committed"},{"retry",true},{"native_outcome",session.outcome},
            {"operation",session.choice.operation},{"target_affix",session.choice.affix_id},{"chosen_replacement",session.choice.replacement_id},
            {"old_engine_id",fname(selected)},{"new_engine_id",item.at("engine_id")},
            {"instance_id",item.at("instance_id")},{"revision",item.at("revision")},
            {"affixes",item.at("affixes")},{"effective_stats",itemization::effective_stats(item)}});
    } catch (const std::exception& error) {log({{"event","forge_replace_refused_or_failed"},{"reason",error.what()}});}
    catch (...) {log({{"event","forge_replace_failed"}});}
    return handled;
}
bool forge_mod_reflect(void* accessor,uint64_t requested,bool equip) noexcept {
    if (!forge_modifiers_enabled) return false;
    bool handled=false;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=forge_sessions.find(reinterpret_cast<uintptr_t>(accessor));
        if (found==forge_sessions.end() || !found->second.modded || found->second.retry) return false;
        handled=true;auto& session=found->second;
        if (!session.prepared || requested!=session.result_id)
            throw std::runtime_error("Forge creation has no matching prepared instance");
        if (session.committed) return true;
        const auto manager=find_manager();
        if (owned_count(manager,session.result_id)!=0) throw std::runtime_error("Forge result already exists in inventory");
        original_forge_reflect(accessor,session.result_id,equip);
        if (owned_count(manager,session.result_id)!=1) throw std::runtime_error("Native forge did not grant exactly one result");
        session.committed=true;
        const auto item=item_service->lookup(runtime_ids.at(session.result_id));
        log({{"event","forge_instance_committed"},{"retry",false},{"native_outcome",session.outcome},
            {"new_engine_id",item.at("engine_id")},{"instance_id",item.at("instance_id")},
            {"revision",item.at("revision")},{"affixes",item.at("affixes")}});
    } catch (const std::exception& error) {log({{"event","forge_grant_refused_or_failed"},{"reason",error.what()}});}
    catch (...) {log({{"event","forge_grant_failed"}});}
    return handled;
}
