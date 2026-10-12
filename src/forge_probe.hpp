// Forge tracing and optional modifier adapters. With forge_modifiers disabled,
// every function passes through unchanged. The bridge owns generated-item results.
bool forge_trace_enabled=false;
using ForgeItemCheckFn=int(*)(void*,uint64_t);
using ForgeSelectFn=void(*)(void*,int,uint64_t,void*);
using ForgeStartFn=void(*)(void*,bool,int);
using ForgeJudgeFn=int(*)(void*);
using ForgeResultNameFn=uint64_t*(*)(void*,uint64_t*,int);
using ForgeReplaceFn=void(*)(void*,uint64_t);
using ForgeReflectFn=void(*)(void*,uint64_t,bool);
using ForgeGroupFn=Array*(*)(void*,Array*);
using ForgePearlsFn=int(*)(void*,int);
using ForgeRecipeFn=void(*)(void*,uint64_t);
ForgeRecipeFn original_forge_recipe=nullptr;
ForgeItemCheckFn original_forge_quality=nullptr,original_forge_cost=nullptr,original_forge_possible=nullptr;
ForgeSelectFn original_forge_select=nullptr;
ForgeStartFn original_forge_start=nullptr;
ForgeJudgeFn original_forge_judge=nullptr;
ForgeResultNameFn original_forge_result_name=nullptr;
ForgeReplaceFn original_forge_replace=nullptr;
ForgeReflectFn original_forge_reflect=nullptr;
ForgeGroupFn original_forge_group=nullptr;
ForgeGroupFn original_forge_top_disabled=nullptr;
ForgePearlsFn original_forge_pearls=nullptr;
#include "forge_bridge.hpp"

Json forge_context(void* accessor) {
    const auto address=reinterpret_cast<uintptr_t>(accessor);
    return {{"game_state",read<uint8_t>(address+0x44)},
        {"recipe_name",fname(read<uint64_t>(address+0x220))},
        {"result",read<int32_t>(address+0x41C)},
        {"retry_flag",read<uint8_t>(address+0x420)},
        {"retry_base_quality",read<int32_t>(address+0x428)},
        {"retry_bag",read<int32_t>(address+0x42C)},
        {"retry_base_item_id",fname(read<uint64_t>(address+0x430))}};
}
void trace_forge_check(const char* check,void* accessor,uint64_t id,int result) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        const auto label=fname(id);const auto key=std::string(check)+":"+label;
        static std::map<std::string,int> previous;
        auto found=previous.find(key);
        if (found!=previous.end() && found->second==result) return;
        previous[key]=result;
        log({{"event","forge_native_check"},{"check",check},{"item_id",label},
            {"native_result",result},{"context",forge_context(accessor)},
            {"behavior_changed",forge_modifiers_enabled}});
    } catch (...) {}
}
int forge_quality_probe(void* accessor,uint64_t id) {
    if (forge_modifiers_enabled) recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
    if(forge_read_base_cost) return 0;
    const auto result=forge_custom_item(id)?0:original_forge_quality(accessor,id);
    trace_forge_check("quality",accessor,id,result);return result;
}
int forge_cost_probe(void* accessor,uint64_t id) {
    if(camp_policy && forge_custom_item(id)) {
        recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
        try {const int cost=forge_base_pearl_cost(accessor,id);trace_forge_check("pearl_cost",accessor,id,cost);return cost;}
        catch(const std::exception& error) {log({{"event","quality_cost_unavailable"},{"reason",error.what()}});return -1;}
    }
    auto result=original_forge_cost(accessor,id);
    if (forge_custom_item(id)) {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
        if(veyl_policy && (!camp_policy || camp_mode!=itemization::CampMode::Quality)) return 0;
        auto plan=forge_plans.find(recent_forge_accessor);
        if (plan!=forge_plans.end() && plan->second.source==id) result=plan->second.cost;
    }
    trace_forge_check("pearl_cost",accessor,id,result);return result;
}
int forge_possible_probe(void* accessor,uint64_t id) {
    if(veyl_policy && !forge_custom_item(id)) return 4; // Legacy/native rows cannot enter the retired affix minigame.
    auto result=original_forge_possible(accessor,id);
    if (forge_custom_item(id)) {
        try {
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
            if(camp_policy && camp_mode==itemization::CampMode::Salvage && !salvage_source_valid(find_manager(),id)) return 4;
            if(camp_policy) return original_forge_pearls(accessor,0)>=forge_base_pearl_cost(accessor,id)?0:1;
            if(veyl_policy) return 0; // Let the currency menu explain unavailable actions.
            const auto native_cost=original_forge_cost(accessor,id);
            const int required=native_cost>0?forge_navigation_cache().minimum_cost(runtime_ids.at(id),native_cost):0;
            if(required==0) result=1;
            else if(original_forge_pearls(accessor,0)>=required) result=0;
            else if(result==0) result=1; // Unavailable; retain native error codes otherwise.
        } catch (...) {result=4;}
    }
    trace_forge_check("eligibility",accessor,id,result);return result;
}
void forge_select_probe(void* accessor,int bag,uint64_t id,void* detail) {
    original_forge_select(accessor,bag,id,detail);
    if(!veyl_policy || camp_policy) forge_mod_select(accessor,id);
    try {log({{"event","forge_native_select"},{"item_id",fname(id)},
        {"bag",bag},{"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
}
void forge_start_probe(void* accessor,bool retry,int quality) {
    if(camp_policy) camp_mode=itemization::CampMode::Quality;
    if(veyl_policy && retry && (!camp_policy || camp_mode!=itemization::CampMode::Quality)) {
        log({{"event","camp_minigame_start_blocked"},{"reason","Currency/salvage actions cannot start forging"}});return;
    }
    original_forge_start(accessor,retry,quality);
    forge_mod_start(accessor,retry);
    try {log({{"event","forge_native_start"},{"retry",retry},{"quality_argument",quality},
        {"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
}
int forge_judge_probe(void* accessor) {
    const auto native_result=original_forge_judge(accessor);int result=native_result;
    if(forge_modifiers_enabled && forge_policy && forge_policy->uses_performance()) {
        try {
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            const auto address=reinterpret_cast<uintptr_t>(accessor);auto found=forge_sessions.find(address);
            if(found!=forge_sessions.end() && ((found->second.retry && found->second.modded) || (camp_policy && !found->second.retry))) {
                auto& session=found->second;
                if(!session.prepared) {
                    std::vector<itemization::ForgeGauge> gauges;Json gauge_values=Json::array();
                    // Owner build: eight final gauge records, accessed by native
                    // GetGaugeInfo (0x921FF0). Read on the game's judge callback;
                    // never alter recipe geometry or gauge values.
                    for(int i=0;i<8;++i) {
                        const auto gauge=address+0x24C+uintptr_t(i)*0x28;
                        const auto active=read<float>(gauge+0x10);
                        if(!std::isfinite(active) || std::abs(active)>1000000) throw std::runtime_error("Invalid native forge gauge marker");
                        if(active<=0) continue; // Match native inactive-gauge test.
                        gauges.push_back({read<float>(gauge),read<float>(gauge+4),read<float>(gauge+8),read<float>(gauge+0xC)});
                        const auto& values=gauges.back();gauge_values.push_back({{"index",i},{"lower",values.lower},{"upper",values.upper},{"target",values.target},{"value",values.value}});
                    }
                    session.performance=forge_policy->measure(gauges);session.performance_measured=true;
                    if(camp_policy) {
                        result=native_result;
                        log({{"event","quality_forge_evaluated"},{"score",session.performance.score},{"quality_bp",itemization::quality_from_score(session.performance.score)},
                            {"retry",session.retry},{"gauges",gauge_values}});
                    } else {
                    const auto& item=item_service->lookup(runtime_ids.at(session.old_id));
                    const auto decision=forge_policy->performance_decision(*item_service,item,session.choice,session.performance.score);
                    result=decision.result;
                    log({{"event","forge_performance_evaluated"},{"engine_id",item.at("engine_id")},{"operation",session.choice.operation},
                        {"target_affix",session.choice.affix_id},{"native_result",native_result},{"decision",decision.details()},
                        {"active_gauges",session.performance.gauges},{"exact_gauges",session.performance.exact},
                        {"worst_deviation",session.performance.worst_deviation},{"gauges",gauge_values}});
                    }
                } else result=session.outcome;
                *reinterpret_cast<int32_t*>(address+0x41C)=result;
            }
        } catch(const std::exception& error) {
            // No edit is allowed without measured performance. Leave the saved
            // item intact rather than fall back to an unrelated native grade.
            result=0;*reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(accessor)+0x41C)=result;
            log({{"event","forge_performance_refused"},{"reason",error.what()}});
        }
    }
    forge_mod_judged(accessor,result);
    try {log({{"event","forge_native_judge"},{"native_result",native_result},{"resolved_result",result},
        {"context",forge_context(accessor)},{"behavior_changed",result!=native_result}});} catch (...) {}
    return result;
}
uint64_t* forge_result_name_probe(void* accessor,uint64_t* output,int index) {
    auto result=original_forge_result_name(accessor,output,index);
    try {
        const auto id=read<uint64_t>(reinterpret_cast<uintptr_t>(result));
        const auto event=Json{{"event","forge_native_result_name"},{"index",index},
            {"item_id",fname(id)},{"context",forge_context(accessor)},{"behavior_changed",false}};
        static thread_local std::string previous;
        auto signature=event.dump();if (signature!=previous) {previous=signature;log(event);}
    } catch (...) {}
    return result;
}
void forge_replace_probe(void* accessor,uint64_t id) {
    try {log({{"event","forge_native_replace_before"},{"result_item_id",fname(id)},
        {"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
    if (!forge_mod_replace(accessor,id)) original_forge_replace(accessor,id);
    try {log({{"event","forge_native_replace_after"},{"result_item_id",fname(id)},
        {"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
}
void forge_reflect_probe(void* accessor,uint64_t id,bool equip) {
    try {log({{"event","forge_native_reflect_before"},{"result_item_id",fname(id)},
        {"equip",equip},{"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
    if (!forge_mod_reflect(accessor,id,equip)) original_forge_reflect(accessor,id,equip);
    try {log({{"event","forge_native_reflect_after"},{"result_item_id",fname(id)},
        {"context",forge_context(accessor)},{"behavior_changed",false}});} catch (...) {}
}
struct ForgeProbeBinding {
    uintptr_t rva;
    std::array<uint8_t,5> signature;
    void* adapter;
    void** original;
};
Array* forge_group_probe(void* accessor,Array* output) {
    if(veyl_policy) recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
    auto result=original_forge_group(accessor,output);
    try {
        auto array=read<Array>(reinterpret_cast<uintptr_t>(result));
        if (array.count<0 || array.count>16 || array.capacity<array.count) return result;
        Json ids=Json::array();for (int i=0;i<array.count;++i) ids.push_back(fname(read<uint64_t>(array.data+uintptr_t(i)*8)));
        auto event=Json{{"event","forge_result_group_read"},{"items",ids},{"context",forge_context(accessor)},
            {"behavior_changed",forge_modifiers_enabled}};
        static thread_local std::string previous;
        auto signature=event.dump();if (signature!=previous) {previous=signature;log(event);}
    } catch (...) {}
    return result;
}
void forge_recipe_probe(void* accessor,uint64_t recipe) {
    original_forge_recipe(accessor,recipe);
    if(veyl_policy) recent_forge_accessor=reinterpret_cast<uintptr_t>(accessor);
}
int forge_pearls_probe(void* accessor,int amount) {
    if(camp_policy && amount==0) camp_wallet_update(reinterpret_cast<uintptr_t>(accessor));
    if(veyl_policy && !camp_policy && amount==0) {
        try {
            // This decorative header is polled every frame. Actual confirmations
            // and commits always read fresh balances; avoid repeated bag scans here.
            static thread_local uint64_t last=0;static thread_local int total=0;
            const auto now=GetTickCount64();if(last && now-last<100) return total;
            const auto wallet=veyl_wallet(find_manager());total=0;for(const auto& q:wallet) total+=q.get<int>();last=now;return total;
        }
        catch(...) {} // A menu before inventory is available keeps the native display.
    }
    const auto before=original_forge_pearls(accessor,0);
    const auto requested=amount;
    if (amount<0 && forge_modifiers_enabled) {
        try {
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            auto found=forge_sessions.find(reinterpret_cast<uintptr_t>(accessor));
            if (found!=forge_sessions.end() && found->second.retry &&
                forge_custom_item(read<uint64_t>(reinterpret_cast<uintptr_t>(accessor)+0x430))) {
                auto& session=found->second;
                const int native_cost=camp_policy?forge_base_pearl_cost(accessor,session.old_id):original_forge_cost(accessor,session.old_id);
                if (!session.modded || session.choice.operation.empty() || session.paid || before<session.pearl_cost ||
                    (amount!=-native_cost && amount!=-session.pearl_cost))
                    throw std::runtime_error("Generated rework has no valid affordable pearl charge");
                // The Blueprint may cache its vanilla cost before our prompt.
                // Charge exactly the displayed operation cost once at this boundary.
                amount=-session.pearl_cost;
            }
        } catch (const std::exception& error) {
            log({{"event","targeted_forge_charge_refused"},{"reason",error.what()}});return before;
        }
    }
    const auto after=original_forge_pearls(accessor,amount);
    if(amount<0 && forge_modifiers_enabled) {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=forge_sessions.find(reinterpret_cast<uintptr_t>(accessor));
        if(found!=forge_sessions.end() && found->second.retry && found->second.modded && amount==-found->second.pearl_cost)
            found->second.paid=before-after==found->second.pearl_cost;
    }
    if (amount!=0) try {log({{"event","forge_native_pearl_change"},{"amount",amount},
        {"native_requested_amount",requested},{"before",before},{"after",after},{"context",forge_context(accessor)},{"behavior_changed",amount!=requested}});} catch (...) {}
    return after;
}
#include "forge_ui_bridge.hpp"
Array* forge_top_disabled_probe(void* accessor,Array* output) {
    const auto result=original_forge_top_disabled(accessor,output);
    // Preserve native recent-recipe state and allocation ownership, but its
    // category indices must never disable unrelated custom campsite actions.
    if(camp_policy && output) output->count=0;
    return result;
}
const std::array<ForgeProbeBinding,21>& forge_probe_bindings() {
    static const std::array<ForgeProbeBinding,21> bindings{{
        {0x928640,{0x40,0x53,0x55,0x56,0x57},reinterpret_cast<void*>(&forge_top_disabled_probe),reinterpret_cast<void**>(&original_forge_top_disabled)},
        {0x931810,{0x48,0x89,0x54,0x24,0x10},reinterpret_cast<void*>(&forge_recipe_probe),reinterpret_cast<void**>(&original_forge_recipe)},
        {0x922460,{0x48,0x89,0x54,0x24,0x10},reinterpret_cast<void*>(&forge_quality_probe),reinterpret_cast<void**>(&original_forge_quality)},
        {0x925F30,{0x48,0x89,0x54,0x24,0x10},reinterpret_cast<void*>(&forge_cost_probe),reinterpret_cast<void**>(&original_forge_cost)},
        {0x928910,{0x48,0x89,0x54,0x24,0x10},reinterpret_cast<void*>(&forge_possible_probe),reinterpret_cast<void**>(&original_forge_possible)},
        {0x9310B0,{0x48,0x89,0x5C,0x24,0x08},reinterpret_cast<void*>(&forge_select_probe),reinterpret_cast<void**>(&original_forge_select)},
        {0x9317F0,{0x84,0xD2,0x88,0x91,0x20},reinterpret_cast<void*>(&forge_start_probe),reinterpret_cast<void**>(&original_forge_start)},
        {0x92BC50,{0x48,0x89,0x5C,0x24,0x10},reinterpret_cast<void*>(&forge_judge_probe),reinterpret_cast<void**>(&original_forge_judge)},
        {0x920540,{0x48,0x8B,0xC1,0x44,0x3B},reinterpret_cast<void*>(&forge_result_name_probe),reinterpret_cast<void**>(&original_forge_result_name)},
        {0x92CF00,{0x48,0x89,0x54,0x24,0x10},reinterpret_cast<void*>(&forge_replace_probe),reinterpret_cast<void**>(&original_forge_replace)},
        {0x92CA90,{0x48,0x89,0x5C,0x24,0x18},reinterpret_cast<void*>(&forge_reflect_probe),reinterpret_cast<void**>(&original_forge_reflect)},
        {0x920570,{0x48,0x89,0x5C,0x24,0x08},reinterpret_cast<void*>(&forge_group_probe),reinterpret_cast<void**>(&original_forge_group)},
        {0x91A280,{0x48,0x89,0x5C,0x24,0x08},reinterpret_cast<void*>(&forge_pearls_probe),reinterpret_cast<void**>(&original_forge_pearls)},
        {0xA0B490,{0x48,0x89,0x5C,0x24,0x10},reinterpret_cast<void*>(&forge_prompt_init_hook),reinterpret_cast<void**>(&original_forge_prompt_init)},
        {0x90D3E0,{0x48,0x8B,0xC4,0x48,0x89},reinterpret_cast<void*>(&forge_prompt_text_hook),reinterpret_cast<void**>(&original_forge_prompt_text)},
        {0xA10710,{0x48,0x89,0x5C,0x24,0x08},reinterpret_cast<void*>(&forge_prompt_control_hook),reinterpret_cast<void**>(&original_forge_prompt_control)},
        {0xA107F0,{0x40,0x53,0x48,0x83,0xEC},reinterpret_cast<void*>(&forge_prompt_widget_hook),reinterpret_cast<void**>(&original_forge_prompt_widget)},
        {0xA046F0,{0x40,0x57,0x48,0x83,0xEC},reinterpret_cast<void*>(&forge_prompt_close_hook),reinterpret_cast<void**>(&forge_prompt_close)},
        {0xA024E0,{0x48,0x89,0x5C,0x24,0x10},reinterpret_cast<void*>(&forge_prompt_append_hook),reinterpret_cast<void**>(&original_forge_prompt_append)},
        {0xA0B390,{0x48,0x89,0x5C,0x24,0x10},reinterpret_cast<void*>(&forge_prompt_fixed_init_hook),reinterpret_cast<void**>(&original_forge_prompt_fixed_init)},
        {0xA023C0,{0x48,0x89,0x5C,0x24,0x10},reinterpret_cast<void*>(&forge_prompt_fixed_append_hook),reinterpret_cast<void**>(&original_forge_prompt_fixed_append)}}};
    return bindings;
}
void create_forge_probe_hooks() {
    for (const auto& binding:forge_probe_bindings())
        for (size_t i=0;i<binding.signature.size();++i)
            if (read<uint8_t>(base+binding.rva+i)!=binding.signature[i])
                throw std::runtime_error("Forge tracing signature mismatch");
    for (const auto& binding:forge_probe_bindings())
        if (MH_CreateHook(reinterpret_cast<void*>(base+binding.rva),binding.adapter,binding.original)!=MH_OK) {
            MH_Uninitialize();throw std::runtime_error("Cannot create forge tracing hook");
        }
}
