#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <MinHook/MinHook.h>
#include <nlohmann/json.hpp>
#include "item_lifecycle.hpp"
#include "item_description.hpp"
#include "crafting_input.hpp"
#include "equipment_lock_menu.hpp"
#include "crafting_presentation.hpp"
#include "crafting_changes.hpp"
#include "forge_policy.hpp"
#include "forge_home.hpp"
#include "forge_menu.hpp"
#include "forge_navigation.hpp"
#include "forge_feedback.hpp"
#include "native_object_handle.hpp"
#include "equipment_stats.hpp"
#include "native_combat_scalars.hpp"
#include "native_added_elements.hpp"
#include "native_combat_dispatch.hpp"
#include "regional_loot.hpp"
#include "regional_loot_native.hpp"
#include "veyl.hpp"
#include "camp_crafting.hpp"
#include "crafting_screen.hpp"
#include "crafting_currency_layout.hpp"
#include "crafting_inventory.hpp"
#include "tickington_skip.hpp"
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <cstring>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Json=nlohmann::json;
HMODULE self=nullptr;
uintptr_t base=0,names=0;
bool indirect_names=false;
std::mutex log_mutex;
std::ofstream log_file;
std::atomic<uint64_t> events{0};
Json prototype;
std::filesystem::path request_path;
std::atomic<bool> granting{false};
template<class T> T read(uintptr_t address) {
    T value{}; SIZE_T count=0;
    if (address<0x10000 || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&count) || count!=sizeof(value))
        throw std::runtime_error("Unusable game pointer");
    return value;
}
std::string name(int32_t index) {
    if (index<0 || index>=4*1024*1024) throw std::runtime_error("Invalid game name index");
    auto pool=indirect_names?read<uintptr_t>(names):names;
    auto chunk=read<uintptr_t>(pool+uintptr_t(index/16384)*8);
    auto entry=read<uintptr_t>(chunk+uintptr_t(index%16384)*8);
    std::string text;
    if (read<uint32_t>(entry)&1) {
        std::wstring wide;
        for (int i=0;i<1024;++i) {auto c=read<wchar_t>(entry+16+i*2);if (!c) break;wide.push_back(c);}
        int count=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
        text.resize(count);if (count) WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),text.data(),count,nullptr,nullptr);
    } else {
        for (int i=0;i<1024;++i) {auto c=read<char>(entry+16+i);if (!c) break;text.push_back(c);}
    }
    return text;
}
std::string fname(uint64_t value) {
    auto text=name(static_cast<int32_t>(value&0xFFFFFFFFULL));
    auto number=static_cast<int32_t>(value>>32);
    if (number>0) text+="_"+std::to_string(number-1);
    return text;
}
void log(Json data) noexcept {
    try {
        data["sequence"]=++events; data["elapsed_ms"]=GetTickCount64();
        std::lock_guard<std::mutex> lock(log_mutex);
        log_file<<data.dump()<<'\n';log_file.flush();
    } catch (...) { /* Diagnostics must never interrupt the game's original operation. */ }
}
using StoreItemFn=int(*)(void*,uint64_t,int,bool,uint8_t*);
StoreItemFn original_store=nullptr;
std::atomic<bool> runtime_enabled{false};
int runtime_acquire(void* manager,uint64_t item,int count,bool overflow,uint8_t* bag);
void remember_item_manager(uintptr_t manager);
void prepare_veyl_inventory(void* manager);
void remember_kingmaker_acquisition(uintptr_t manager,uint64_t item,int accepted);
int store_hook(void* manager,uint64_t item,int count,bool overflow,uint8_t* bag) {
    if(runtime_enabled.load()) try {remember_item_manager(reinterpret_cast<uintptr_t>(manager));} catch(...) {}
    if(runtime_enabled.load()) try {prepare_veyl_inventory(manager);} catch(const std::exception& e) {log({{"event","veyl_stack_setup_failed"},{"reason",e.what()}});}
    int result=runtime_enabled.load()?runtime_acquire(manager,item,count,overflow,bag):original_store(manager,item,count,overflow,bag);
    if(runtime_enabled.load())try {remember_kingmaker_acquisition(reinterpret_cast<uintptr_t>(manager),item,count-result);}
    catch(const std::exception& e){log({{"event","kingmaker_discovery_failed"},{"reason",e.what()}});}
    try {
        log({{"event","item_acquisition"},{"base_id",fname(item)},{"requested_count",count},
            {"original_result",result},{"returned_bag",bag?Json(read<uint8_t>(reinterpret_cast<uintptr_t>(bag))):Json(nullptr)}});
    } catch (...) {}
    return result;
}
struct Array {uintptr_t data;int32_t count,capacity;};
using EquippedFn=Array*(*)(void*);
EquippedFn original_equipped=nullptr;
std::mutex manager_cache_mutex;
itemization::NativeObjectHandle cached_item_manager,cached_manager_owner;
std::atomic<uint64_t> manager_cache_hits{0},manager_full_scans{0};
uintptr_t resolve_manager_handle(const itemization::NativeObjectHandle& handle) {
    const auto count=read<int32_t>(base+0x5D83BF8+0x1C);
    if(handle.index<0 || handle.index>=count) return 0;
    const auto slot=read<uintptr_t>(base+0x5D83BF8+0x10)+uintptr_t(handle.index)*24;
    const auto pointer=read<uintptr_t>(slot);
    if(pointer!=handle.address || !pointer) return 0;
    return handle.matches(pointer,read<int32_t>(slot+0x10),read<uint32_t>(slot+8),read<uint32_t>(pointer+8))?pointer:0;
}
itemization::NativeObjectHandle manager_handle(uintptr_t object) {
    const auto index=read<int32_t>(object+0xC),count=read<int32_t>(base+0x5D83BF8+0x1C);
    if(index<0 || index>=count) throw std::runtime_error("Invalid manager object slot");
    const auto slot=read<uintptr_t>(base+0x5D83BF8+0x10)+uintptr_t(index)*24;
    itemization::NativeObjectHandle handle{index,read<int32_t>(slot+0x10),object};
    if(!resolve_manager_handle(handle)) throw std::runtime_error("Manager object is not live");
    return handle;
}
bool manager_object_is_a(uintptr_t object,const char* wanted) {
    auto cls=read<uintptr_t>(object+0x10);
    for(int depth=0;cls && depth<32;++depth,cls=read<uintptr_t>(cls+0x40))
        if(fname(read<uint64_t>(cls+0x18))==wanted) return true;
    return false;
}
void cache_item_manager_locked(uintptr_t manager) {
    const auto owner=read<uintptr_t>(manager+0x20);
    if(!owner || !manager_object_is_a(manager,"JackItemManager") || !manager_object_is_a(owner,"GameInstance"))
        throw std::runtime_error("Unsupported item manager owner");
    auto handle=manager_handle(manager),owner_handle=manager_handle(owner);
    cached_item_manager=handle;cached_manager_owner=owner_handle;
}
void remember_item_manager(uintptr_t manager) {
    std::lock_guard<std::mutex> lock(manager_cache_mutex);
    if(manager==cached_item_manager.address && resolve_manager_handle(cached_item_manager) && resolve_manager_handle(cached_manager_owner) &&
       read<uintptr_t>(manager+0x20)==cached_manager_owner.address) return;
    cache_item_manager_locked(manager);
    log({{"event","item_manager_cache_warmed"},{"source","native item-manager callback"},
        {"index",cached_item_manager.index},{"serial",cached_item_manager.serial},{"owner_index",cached_manager_owner.index}});
}
uintptr_t find_manager() {
    std::lock_guard<std::mutex> lock(manager_cache_mutex);
    if(const auto manager=resolve_manager_handle(cached_item_manager)) {
        if(const auto owner=resolve_manager_handle(cached_manager_owner)) {
            if(read<uintptr_t>(manager+0x20)==owner) {++manager_cache_hits;return manager;}
        }
    }
    cached_item_manager={};cached_manager_owner={};
    const auto started=GetTickCount64();++manager_full_scans;
    auto objects=read<uintptr_t>(base+0x5D83BF8+0x10);
    auto count=read<int32_t>(base+0x5D83BF8+0x1C);
    if (count<=0 || count>4000000) throw std::runtime_error("Invalid object array");
    std::vector<uintptr_t> managers;
    std::unordered_map<uintptr_t,bool> manager_classes;
    for (int i=0;i<count;++i) {
        try {
            auto slot=objects+uintptr_t(i)*24;
            if (read<uint32_t>(slot+8)&((1U<<28)|(1U<<29))) continue;
            auto object=read<uintptr_t>(slot);
            if (!object || read<uint32_t>(object+8)&(0x10U|0x20U|0x8000U|0x10000U)) continue;
            auto cls=read<uintptr_t>(object+0x10);
            auto cached=manager_classes.find(cls);
            if (cached!=manager_classes.end()) {
                if (cached->second) managers.push_back(object);
                continue;
            }
            auto root_class=cls;
            bool is_manager=false;
            for (int depth=0;cls && depth<32;++depth) {
                if (fname(read<uint64_t>(cls+0x18))=="JackItemManager") {
                    is_manager=true;break;
                }
                cls=read<uintptr_t>(cls+0x40);
            }
            manager_classes.emplace(root_class,is_manager);
            if (is_manager) managers.push_back(object);
        } catch (const std::exception&) { /* Skip objects being destroyed. */ }
    }
    if (managers.empty()) throw std::runtime_error("Item manager not available");
    if (managers.size()!=1) throw std::runtime_error("Multiple active item managers");
    cache_item_manager_locked(managers.front());
    log({{"event","item_manager_cache_discovered"},{"scan_ms",GetTickCount64()-started},{"full_scans",manager_full_scans.load()},
        {"index",cached_item_manager.index},{"serial",cached_item_manager.serial},{"owner_index",cached_manager_owner.index}});
    return managers.front();
}
int owned_count(uintptr_t manager,uint64_t id) {
    int total=0;
    for (int bag=0;bag<=12;++bag) {
        auto array=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if (array.count<0 || array.count>10000 || array.capacity<array.count || array.capacity>10000)
            throw std::runtime_error("Invalid inventory array");
        for (int i=0;i<array.count;++i) {
            auto item=read<uintptr_t>(array.data+uintptr_t(i)*16);
            if (read<uint64_t>(item+0x10)==id) {
                int quantity=read<int32_t>(item+0x18);
                if (quantity<0 || quantity>10000) throw std::runtime_error("Invalid inventory quantity");
                total+=quantity;
            }
        }
    }
    return total;
}
#include "lifetime_bridge.hpp"
#include "regional_loot_bridge.hpp"
#include "tickington_skip_bridge.hpp"
#include "combat_scalar_bridge.hpp"
#include "added_element_bridge.hpp"
#include "combat_dispatch_bridge.hpp"
#include "combat_event_bridge.hpp"
#include "combat_trace_bridge.hpp"
#include "equipment_ui_bridge.hpp"
#include "forge_feedback_bridge.hpp"
#include "camp_wallet_bridge.hpp"
#include "crafting_ui_bridge.hpp"
void maybe_grant_test_items(void* character) noexcept {
    if (prototype.is_null() || granting.load()) return;
    try {
        if (!std::filesystem::exists(request_path)) return;
        if (fname(read<uint64_t>(reinterpret_cast<uintptr_t>(character)+0x38))!="UN90000") return;
        if (granting.exchange(true)) return;
        // This runs inside the original equipment getter's caller, never on the
        // DLL worker or a remote thread. No gameplay function is invoked at load.
        auto consumed=request_path;consumed+=L".consumed";
        if (std::filesystem::exists(consumed)) std::filesystem::remove(consumed);
        std::filesystem::rename(request_path,consumed);
        if (read<int32_t>(reinterpret_cast<uintptr_t>(character)+0x78)!=1)
            throw std::runtime_error("Test grants require the level-1 protagonist");
        auto manager=find_manager();
        auto table=read<uintptr_t>(manager+0x38);
        auto rows=read<Array>(table+0x40);
        if (rows.count!=2734 || rows.capacity<rows.count || rows.capacity>20000)
            throw std::runtime_error("The four-fixture item table is not mounted");
        struct Fixture {uint64_t id;std::string label;};
        std::vector<Fixture> fixtures;
        const std::array<std::pair<const char*,uintptr_t>,7> stats{{
            {"attack",0x98},{"defence",0x9C},{"deftness",0xC0},{"agility",0xC4},
            {"charm",0xD8},{"max_hp",0xD0},{"max_mp",0xD4}}};
        // Preflight all four definitions before granting any item.
        for (const auto& fixture:prototype.at("items")) {
            auto label=fixture.at("game_item_id").get<std::string>();
            bool found=false;
            for (int i=0;i<rows.count;++i) {
                auto slot=rows.data+uintptr_t(i)*24;
                auto id=read<uint64_t>(slot);
                if (fname(id)!=label) continue;
                auto row=read<uintptr_t>(slot+8);
                for (auto [stat,offset]:stats)
                    if (read<int32_t>(row+offset)!=fixture.at("expected_stats").at(stat).get<int>())
                        throw std::runtime_error("A fixture stat differs from its generated roll");
                int existing=owned_count(manager,id);
                if (existing>1) throw std::runtime_error("Fixture already has duplicate copies");
                fixtures.push_back({id,label});found=true;break;
            }
            if (!found) throw std::runtime_error("Missing fixture definition");
        }
        for (const auto& fixture:fixtures) {
            int before=owned_count(manager,fixture.id);
            if (before==1) {log({{"event","test_fixture_already_owned"},{"item_id",fixture.label}});continue;}
            uint8_t bag=255;
            int remaining=original_store(reinterpret_cast<void*>(manager),fixture.id,1,false,&bag);
            int after=owned_count(manager,fixture.id);
            log({{"event","test_fixture_grant"},{"item_id",fixture.label},{"remaining_count",remaining},
                 {"returned_bag",bag},{"owned_before",before},{"owned_after",after},{"save_written",false}});
            if (remaining!=0 || after!=1) throw std::runtime_error("The game did not accept a test fixture");
        }
        log({{"event","test_grant_complete"},{"fixture_count",fixtures.size()},{"save_written",false}});
    } catch (const std::exception& error) {
        log({{"event","test_grant_refused_or_stopped"},{"reason",error.what()},{"save_written",false}});
    } catch (...) { log({{"event","test_grant_failed"},{"save_written",false}}); }
    granting=false;
}
thread_local std::map<std::string,std::string> previous_equipment;
Array* equipped_hook(void* character) {
    auto* result=original_equipped(character);
    maybe_grant_test_items(character);
    maybe_runtime_test(character);
    try {
        auto array=read<Array>(reinterpret_cast<uintptr_t>(result));
        if (array.count<0 || array.count>8 || array.capacity<array.count) return result;
        Json items=Json::array();
        for (int i=0;i<array.count;++i) {
            auto slot=array.data+uintptr_t(i)*16;
            items.push_back({{"base_id",fname(read<uint64_t>(slot))},{"region",read<uint8_t>(slot+8)}});
        }
        Json event{{"event","equipped_items"}, {"character_id",fname(read<uint64_t>(reinterpret_cast<uintptr_t>(character)+0x38))}, {"items",items}};
        auto signature=event.dump();
        auto& previous=previous_equipment[event.at("character_id").get<std::string>()];
        if (signature!=previous) {previous=signature;log(std::move(event));}
    } catch (...) {}
    return result;
}
DWORD WINAPI initialize(void*) {
    try {
        wchar_t module_path[32768]{};
        if (!GetModuleFileNameW(self,module_path,32768)) return 1;
        auto log_path=std::filesystem::path(module_path).parent_path()/L"itemization-probe.jsonl";
        log_file.open(log_path,std::ios::app);
        if (!log_file) return 2;
        base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DRAGON QUEST XI S.exe"));
        if (!base || read<uint64_t>(base+0x36835D0)!=0x37353A38313A3132ULL) {
            log({{"event","unsupported_executable"}});return 3;
        }
        names=base+0x5D7AE20;
        try {if (name(0)!="None") indirect_names=true;} catch (...) {indirect_names=true;}
        if (name(0)!="None") {log({{"event","name_validation_failed"}});return 4;}
        auto folder=std::filesystem::path(module_path).parent_path();
        request_path=folder/L"grant-normal-test.request";
        bool use_runtime=initialize_runtime(folder);
        if(use_runtime) {std::ifstream config(folder/L"runtime-config.json");const auto settings=Json::parse(config);
            initialize_regional_loot(folder,settings);
            combat_trace_enabled=settings.value("combat_trace",false);initialize_combat_scalars(folder,settings);
            added_fire_prototype_enabled=settings.value("native_added_fire_prototype",false);
            initialize_combat_dispatch(folder,settings);
            initialize_combat_events(settings);
            if(added_fire_prototype_enabled && !combat_scalars_enabled) throw std::runtime_error("Added Fire requires native action metadata");
            if(added_fire_prototype_enabled) validate_added_fire_bindings();
            if(added_fire_prototype_enabled) log({{"event","added_fire_adapter_loaded"},{"scope","ordinary noncritical single greatsword hits"},{"gameplay_verified",false}});}
        std::ifstream manifest(folder/L"prototype-manifest.json");
        if (manifest) {
            auto parsed=Json::parse(manifest);
            const std::array<const char*,4> ids{{"W_SWD_ARPG_TEST_A","W_SWD_ARPG_TEST_B","E_CLO_ARPG_TEST_C","W_TSW_ARPG_TEST_D"}};
            if (parsed.at("schema_version")!=1 || parsed.at("steam_build")!=6291077 || parsed.at("items").size()!=4)
                throw std::runtime_error("Unexpected prototype manifest");
            for (size_t i=0;i<ids.size();++i) {
                const auto& fixture=parsed.at("items").at(i);
                if (fixture.at("game_item_id")!=ids[i] || fixture.at("roll").at("rarity")!="normal" ||
                    fixture.at("roll").at("affixes").size()!=1)
                    throw std::runtime_error("Unexpected test fixture");
            }
            prototype=std::move(parsed);
            log({{"event","prototype_config_loaded"},{"automatic_grant",false},{"fixture_count",4}});
        }
        // Confirm short, inspected prologues before adding hooks to the verified build.
        if (read<uint8_t>(base+0x780E30)!=0x48 || read<uint16_t>(base+0x714CF0)!=0x5340) {
            // Equipped getter begins 40 53 (REX prefix + push rbx) on this Steam executable.
            log({{"event","native_signature_failed"},{"store_byte",read<uint8_t>(base+0x780E30)},
                {"equipment_word",read<uint16_t>(base+0x714CF0)}});return 5;
        }
        auto status=MH_Initialize();
        if (status!=MH_OK) {log({{"event","hook_initialization_failed"},{"status",status}});return 6;}
        auto store_status=MH_CreateHook(reinterpret_cast<void*>(base+0x780E30),reinterpret_cast<void*>(&store_hook),reinterpret_cast<void**>(&original_store));
        auto equip_status=MH_CreateHook(reinterpret_cast<void*>(base+0x714CF0),reinterpret_cast<void*>(&equipped_hook),reinterpret_cast<void**>(&original_equipped));
        if (store_status!=MH_OK || equip_status!=MH_OK) {
            MH_Uninitialize();log({{"event","hook_creation_failed"},{"store_status",store_status},{"equipment_status",equip_status}});return 7;
        }
        if (use_runtime) {
            const std::array<std::pair<uintptr_t,std::array<uint8_t,5>>,7> signatures{{
                {0x764BB0,{0x48,0x89,0x54,0x24,0x10}},
                {0x771480,{0x48,0x89,0x5C,0x24,0x08}},
                {0x770060,{0x48,0x89,0x5C,0x24,0x10}},
                {0x714D10,{0x48,0x89,0x5C,0x24,0x08}},
                {0x93A600,{0x48,0x89,0x5C,0x24,0x18}},
                {0x93A850,{0x48,0x89,0x5C,0x24,0x08}},
                {0x8233D0,{0x4C,0x89,0x44,0x24,0x18}}}};
            for (const auto& signature:signatures)
                for (size_t i=0;i<signature.second.size();++i)
                    if (read<uint8_t>(base+signature.first+i)!=signature.second[i])
                        throw std::runtime_error("Runtime binding signature mismatch");
            auto row_status=MH_CreateHook(reinterpret_cast<void*>(base+0x764BB0),reinterpret_cast<void*>(&item_row_hook),reinterpret_cast<void**>(&original_item_row));
            auto name_status=MH_CreateHook(reinterpret_cast<void*>(base+0x771480),reinterpret_cast<void*>(&item_name_hook),reinterpret_cast<void**>(&original_item_name));
            auto description_status=MH_CreateHook(reinterpret_cast<void*>(base+0x770060),reinterpret_cast<void*>(&description_hook),reinterpret_cast<void**>(&original_description));
            auto parameter_status=MH_CreateHook(reinterpret_cast<void*>(base+0x714D10),reinterpret_cast<void*>(&equipped_parameters_hook),reinterpret_cast<void**>(&original_equipped_parameters));
            auto shop_buy_status=MH_CreateHook(reinterpret_cast<void*>(base+0x93A600),reinterpret_cast<void*>(&shop_buy_hook),reinterpret_cast<void**>(&original_shop_buy));
            auto shop_delivery_status=MH_CreateHook(reinterpret_cast<void*>(base+0x93A850),reinterpret_cast<void*>(&shop_delivery_hook),reinterpret_cast<void**>(&original_shop_delivery));
            auto macro_status=MH_CreateHook(reinterpret_cast<void*>(base+0x8233D0),reinterpret_cast<void*>(&macro_replace_hook),reinterpret_cast<void**>(&original_macro_replace));
            if (row_status!=MH_OK || name_status!=MH_OK || description_status!=MH_OK || parameter_status!=MH_OK || shop_buy_status!=MH_OK || shop_delivery_status!=MH_OK || macro_status!=MH_OK) {
                MH_Uninitialize();throw std::runtime_error("Runtime hook creation failed");
            }
            if (forge_trace_enabled || forge_modifiers_enabled) create_forge_probe_hooks();
            create_equipment_ui_hooks();
            create_regional_loot_hook();
            create_tickington_skip_hooks();
            create_forge_feedback_hook();
            create_crafting_ui_hook();
            enable_regional_loot_hook();
            if(combat_trace_enabled || combat_scalars_enabled || added_fire_prototype_enabled || combat_dispatch_enabled) create_combat_trace_hook();
            if(combat_dispatch_enabled) {create_dispatch_hooks();create_combat_event_hooks();}
            if(MH_EnableHook(reinterpret_cast<void*>(base+0x934AD0))!=MH_OK)
                throw std::runtime_error("Forge feedback activation failed");
            if(combat_trace_enabled || combat_scalars_enabled || added_fire_prototype_enabled || combat_dispatch_enabled) for(const auto offset:{0x3DA470,0x3DBF20})
                if(MH_EnableHook(reinterpret_cast<void*>(base+offset))!=MH_OK)
                    throw std::runtime_error("Combat trace activation failed");
            if(combat_dispatch_enabled) {enable_dispatch_hooks();enable_combat_event_hooks();}
            for (auto offset:{0x764BB0,0x771480,0x770060,0x714D10,0x93A600,0x93A850,0x8233D0})
                if (MH_EnableHook(reinterpret_cast<void*>(base+offset))!=MH_OK) {
                    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();throw std::runtime_error("Runtime hook activation failed");
                }
            if (forge_trace_enabled || forge_modifiers_enabled) for (const auto& binding:forge_probe_bindings())
                if (MH_EnableHook(reinterpret_cast<void*>(base+binding.rva))!=MH_OK) {
                    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();throw std::runtime_error("Forge tracing activation failed");
                }
            for(const auto& binding:equipment_ui_bindings())
                if(MH_EnableHook(reinterpret_cast<void*>(base+binding.first))!=MH_OK) {
                    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();throw std::runtime_error("Equipment UI hook activation failed");
                }
        }
        auto store_enable=MH_EnableHook(reinterpret_cast<void*>(base+0x780E30));
        auto equip_enable=MH_EnableHook(reinterpret_cast<void*>(base+0x714CF0));
        if (store_enable!=MH_OK || equip_enable!=MH_OK) {
            MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();log({{"event","hook_enable_failed"}});return 8;
        }
        runtime_enabled=use_runtime;
        log({{"event","probe_active"},{"steam_build",6291077},{"affixes_applied",use_runtime},
            {"module_update","independent-crafting-v1.16.0"},
            {"forge_trace",forge_trace_enabled},{"forge_behavior_changed",forge_modifiers_enabled},
            {"forge_performance_enabled",forge_modifiers_enabled && forge_policy && forge_policy->uses_performance()},{"save_writes",false}});
        return 0;
    } catch (const std::exception& error) {
        runtime_enabled=false;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
        log({{"event","probe_initialization_failed"},{"reason",error.what()}});return 9;
    } catch (...) {
        runtime_enabled=false;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
        log({{"event","probe_initialization_failed"},{"reason","unknown exception"}});return 9;
    }
}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {
        self=instance;DisableThreadLibraryCalls(instance);
        HANDLE worker=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);
        if (worker) CloseHandle(worker);
    }
    // The diagnostic remains resident until game exit. Never unhook under loader lock.
    return TRUE;
}
