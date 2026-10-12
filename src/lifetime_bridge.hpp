// Included inside native_probe.cpp's private namespace after its read-only helpers.
// Runtime rows are private, non-owning views of a live vanilla row. They never enter
// UDataTable's owned RowMap, so the engine cannot destruct/free their shared strings.
// Each published view is immutable and cached for its table and item revision.
Json acquisition_rarity_weights(void* context);
using ItemRowFn=uintptr_t(*)(void*,uint64_t,void*,bool);
using NameCtorFn=uint64_t*(*)(uint64_t*,const char*,int);
using LiteralNameInitFn=void(*)(uint64_t*,const char*,int,int,bool,int);
using TextNameFn=Array*(*)(void*,Array*,uint64_t,bool);
using DescriptionFn=Array*(*)(void*,Array*,uint64_t);
using PrintfFn=Array*(*)(Array*,const wchar_t*,...);
using EquippedParametersFn=uintptr_t(*)(void*,void*,bool);
using ShopBuyFn=int(*)(void*,bool);
using ShopDeliveryFn=int(*)(void*,int,const uint64_t*,int);
ItemRowFn original_item_row=nullptr;
TextNameFn original_item_name=nullptr;
DescriptionFn original_description=nullptr;
EquippedParametersFn original_equipped_parameters=nullptr;
ShopBuyFn original_shop_buy=nullptr;
ShopDeliveryFn original_shop_delivery=nullptr;
struct ShopPurchase {
    std::vector<uint64_t> delivered_ids;
};
thread_local ShopPurchase* active_shop_purchase=nullptr;
std::unique_ptr<itemization::ItemService> item_service;
std::unordered_map<std::string,Json> runtime_bases;
std::unordered_map<uint64_t,std::string> runtime_ids;
std::unordered_map<std::string,uint64_t> native_base_ids;
struct alignas(16) RowView {std::array<uint8_t,0x1E8> data{};};
std::map<std::pair<uintptr_t,uint64_t>,std::unique_ptr<RowView>> runtime_views;
std::recursive_mutex runtime_mutex;
std::unique_ptr<itemization::Random> runtime_random;
std::unique_ptr<itemization::VeylPolicy> veyl_policy;
std::map<std::string,uint64_t> veyl_native_ids;
std::unique_ptr<itemization::CampCraftPolicy> camp_policy;
std::map<std::string,uint64_t> shard_native_ids;
itemization::CampMode camp_mode=itemization::CampMode::Veyl;
Json runtime_total_caps=Json::object(),runtime_rarity_weights=Json::object();
std::filesystem::path history_path,runtime_request;
uint64_t native_name(const std::string& text) {
    uint64_t result=0;
    reinterpret_cast<NameCtorFn>(base+0xD697D0)(&result,text.c_str(),1); // FNAME_Add
    if (fname(result)!=text) throw std::runtime_error("Engine name construction mismatch");
    return result;
}
uint64_t native_literal_name(const std::string& text) {
    // DQAsset serializes custom table keys as the entire name with Number=0.
    // The normal FName constructor instead splits a trailing _1 into Number=2.
    // Init's audited fifth argument disables that split for authored shard IDs.
    uint64_t result=0;
    reinterpret_cast<LiteralNameInitFn>(base+0xD68EE0)(&result,text.c_str(),0,1,false,-1);
    if((result>>32)!=0 || fname(result)!=text) throw std::runtime_error("Literal item name construction mismatch");
    return result;
}
void persist_history() {
    auto bytes=item_service->export_history().dump();
    std::filesystem::create_directories(history_path.parent_path());
    auto temp=history_path;temp+=L".pending";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write item history");
    bool okay=true;size_t offset=0;
    while (offset<bytes.size()) {
        DWORD written=0,chunk=static_cast<DWORD>(std::min<size_t>(bytes.size()-offset,1<<20));
        if (!WriteFile(file,bytes.data()+offset,chunk,&written,nullptr) || written!=chunk) {okay=false;break;}
        offset+=written;
    }
    if (okay) okay=FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    if (!okay || !MoveFileExW(temp.c_str(),history_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot durably commit item history");
}
uint64_t record_runtime_item(const Json& item) {
    // Commit the resolved snapshot before the game can acquire or save its ID.
    auto previous=item_service->export_history();
    try {
        item_service->record(item);persist_history();
        auto id=native_name(item.at("engine_id"));runtime_ids.emplace(id,item.at("engine_id").get<std::string>());
        return id;
    } catch (...) {item_service->import_history(previous);throw;}
}
const std::map<std::string,std::pair<size_t,int>>& point_bindings() {
    static const auto bindings=[] {std::map<std::string,std::pair<size_t,int>> result;
        for(const auto& binding:itemization::equipment_stat_bindings()) if(!binding.percentage)
            result.emplace(binding.stat,std::make_pair(binding.offset,binding.parameter));return result;}();
    return bindings;
}
const std::map<std::string,std::pair<size_t,int>>& percent_bindings() {
    static const auto bindings=[] {std::map<std::string,std::pair<size_t,int>> result;
        for(const auto& binding:itemization::equipment_stat_bindings()) if(binding.percentage)
            result.emplace(binding.stat,std::make_pair(binding.offset,binding.parameter));return result;}();
    return bindings;
}
uintptr_t runtime_row(void* table,uint64_t id,void* context,bool warn) {
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    auto found=runtime_ids.find(id);
    if (found==runtime_ids.end()) return original_item_row(table,id,context,warn);
    const auto& item=item_service->lookup(found->second);
    auto native_id=item.at("base_snapshot").at("native_base_id").get<std::string>();
    auto binding=native_base_ids.find(native_id);
    if (binding==native_base_ids.end()) binding=native_base_ids.emplace(native_id,native_name(native_id)).first;
    auto origin=original_item_row(table,binding->second,context,warn);
    if (!origin) {log({{"event","runtime_base_unavailable"},{"engine_id",found->second}});return 0;}
    auto key=std::make_pair(reinterpret_cast<uintptr_t>(table),id);
    auto present=runtime_views.find(key);
    if (present!=runtime_views.end()) return reinterpret_cast<uintptr_t>(present->second->data.data());
    // Publish a fully populated immutable view. Another game thread may be reading
    // a previously returned pointer, so never copy/patch a published buffer again.
    auto view=std::make_unique<RowView>();
    SIZE_T copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(origin),view->data.data(),view->data.size(),&copied) || copied!=view->data.size())
        throw std::runtime_error("Cannot read native equipment base");
    auto stats=itemization::effective_stats(item);
    itemization::write_equipment_stats(view->data.data(),view->data.size(),stats);
    for (const auto& effect:item.at("affixes")) {
        std::string stat=effect.at("stat");
        auto point=point_bindings().find(stat);auto percent=percent_bindings().find(stat);
        if(point==point_bindings().end() && percent==percent_bindings().end()) {
            // Derived item-local effects expose their resolved native attribute.
            if(stat=="attack_percent") point=point_bindings().find("attack");
            else if(stat=="defence_percent") point=point_bindings().find("defence");
            else if(stat=="mp_absorption_wand" || stat=="mp_absorption_heavy_wand") percent=percent_bindings().find("mp_absorption");
            else continue;
        }
        int parameter=point!=point_bindings().end()?point->second.second:percent->second.second;
        auto first=view->data.begin()+0xDC,last=first+4;
        if (std::find(first,last,parameter)!=last) continue;
        auto empty=std::find(first,last,0);if (empty!=last) *empty=static_cast<uint8_t>(parameter);
    }
    auto result=reinterpret_cast<uintptr_t>(view->data.data());
    runtime_views.emplace(key,std::move(view));return result;
}
uintptr_t item_row_hook(void* table,uint64_t id,void* context,bool warn) {
    try {if (runtime_enabled.load()) return runtime_row(table,id,context,warn);}
    catch (const std::exception& error) {log({{"event","runtime_row_failed"},{"reason",error.what()}});}
    return original_item_row(table,id,context,warn);
}
std::wstring wide_utf8(const std::string& text) {
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (length<=0) throw std::runtime_error("Invalid runtime text");
    std::wstring result(length,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),length);
    return result;
}
Array* runtime_text(Array* destination,const std::string& text) {
    auto wide=wide_utf8(text);
    return reinterpret_cast<PrintfFn>(base+0xCAAC00)(destination,L"%s",wide.c_str());
}
#include "message_bridge.hpp"
Array* item_name_hook(void* manager,Array* output,uint64_t id,bool list) {
    if(runtime_enabled.load()) try {remember_item_manager(reinterpret_cast<uintptr_t>(manager));} catch(...) {}
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=runtime_ids.find(id);
        if (runtime_enabled.load() && found!=runtime_ids.end())
            return runtime_text(output,itemization::instance_name(item_service->lookup(found->second)));
    } catch (const std::exception& error) {log({{"event","runtime_name_failed"},{"reason",error.what()}});}
    return original_item_name(manager,output,id,list);
}
std::string number_text(const Json& value) {
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    auto text=std::to_string(value.get<double>());
    while (!text.empty() && text.back()=='0') text.pop_back();
    if (!text.empty() && text.back()=='.') text.pop_back();return text;
}
Array* description_hook(void* manager,Array* output,uint64_t id) {
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=runtime_ids.find(id);
        if (runtime_enabled.load() && found!=runtime_ids.end()) {
            const auto& item=item_service->lookup(found->second);
            return runtime_text(output,itemization::item_description(*item_service,item));
        }
        if(runtime_enabled.load()&&camp_policy)for(const auto& shard:shard_native_ids)if(shard.second==id)
            return runtime_text(output,camp_policy->definition(shard.first).at("description").get<std::string>());
    } catch (const std::exception& error) {log({{"event","runtime_description_failed"},{"reason",error.what()}});}
    return original_description(manager,output,id);
}
uintptr_t equipped_parameters_hook(void* character,void* output,bool handedness) {
    auto result=original_equipped_parameters(character,output,handedness);
    try {
        if(runtime_enabled.load() && !runtime_total_caps.empty()) {
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            const auto equipment=read<Array>(reinterpret_cast<uintptr_t>(character)+0x5D8);
            if(equipment.count>=0 && equipment.count<=32 && equipment.capacity>=equipment.count) {
                bool generated=false;
                for(int i=0;i<equipment.count;++i) generated|=runtime_ids.count(read<uint64_t>(equipment.data+uintptr_t(i)*16))!=0;
                if(generated) itemization::clamp_equipped_chances(reinterpret_cast<uint8_t*>(result),0x4C,runtime_total_caps);
            }
        }
        auto id=fname(read<uint64_t>(reinterpret_cast<uintptr_t>(character)+0x38));
        if (id!="UN90000") return result;
        Json values=Json::object();
        // Offsets/types audited from the owner's reflected
        // /Script/JackGame.JackGameCharacterParameters (size 0x4C).
        for (auto [stat,offset]:std::array<std::pair<const char*,uintptr_t>,11>{{
            {"attack",0x24},{"defence",0x28},{"deftness",0xC},{"agility",0x8},
            {"charm",0x10},{"max_hp",0x1C},{"max_mp",0x20},
            {"strength",0x0},{"resilience",0x4},{"magical_might",0x14},{"magical_mending",0x18}}})
            values[stat]=read<int32_t>(result+offset);
        for(auto [stat,offset]:std::array<std::pair<const char*,uintptr_t>,6>{{
            {"mp_absorption",0x30},{"shield_guard_chance",0x34},{"weapon_guard_chance",0x38},
            {"dodge_chance",0x3C},{"critical_chance",0x40},{"zone_chance",0x44}}})
            values[stat]=read<float>(result+offset);
        Json event{{"event","native_equipped_parameters"},{"character_id",id},{"handedness",handedness},{"values",values}};
        static thread_local std::string previous;
        auto signature=event.dump();if (signature!=previous) {previous=signature;log(std::move(event));}
    } catch (...) {}
    return result;
}
std::string new_logical_id() {
    static const char digits[]="0123456789abcdef";
    std::string result;for (int i=0;i<32;++i) result+=digits[runtime_random->below(16)];return result;
}
// Every acquisition adapter uses this path: one base lookup, one fresh ItemService
// roll per unit, durable history, then the adapter's original delivery function.
template<class Delivery,class Fallback>
int generate_and_deliver(void* manager,uint64_t item,int count,const char* source,Delivery deliver,Fallback fallback,
    const std::string& requested_rarity="") {
    int accepted=0;bool started=false;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto label=fname(item);auto definition=runtime_bases.find(label);
        if (count<=0 || count>10000 || definition==runtime_bases.end()) return fallback();
        if (!definition->second.value("automatic_acquisition",true) || std::stoi(definition->second.at("native_fields").at("LimitedNumber").get<std::string>())>0) {
            log({{"event","runtime_kept_vanilla"},{"base_id",label},{"reason","quantity-limited base needs quest integration"}});
            return fallback();
        }
        auto table=read<uintptr_t>(reinterpret_cast<uintptr_t>(manager)+0x38);
        if (!original_item_row(reinterpret_cast<void*>(table),native_name(definition->second.at("native_base_id")),nullptr,false))
            throw std::runtime_error("Runtime equipment base is not loaded");
        for (int i=0;i<count;++i) {
            auto identity=new_logical_id();
            // Shops have a hard Normal-only policy. Development rarity tests use
            // this same generation/delivery path without changing acquisition defaults.
            std::string rarity="normal";
            if(std::strcmp(source,"shop")!=0) {
                if(!requested_rarity.empty()) rarity=requested_rarity;
                else {
                    const auto weights=acquisition_rarity_weights(manager);
                    uint64_t total=0;for(auto i=weights.begin();i!=weights.end();++i) total+=i.value().get<uint32_t>();
                    if(total==0) throw std::runtime_error("Empty acquisition rarity weights");
                    auto selection=runtime_random->below(total);
                    for(auto i=weights.begin();i!=weights.end();++i) {
                        const uint32_t weight=i.value();if(selection<weight) {rarity=i.key();break;}selection-=weight;
                    }
                }
            }
            auto record=item_service->generate(label,rarity,runtime_random->below(UINT64_MAX),identity);
            auto engine_id=record_runtime_item(record);
            if (!runtime_row(reinterpret_cast<void*>(table),engine_id,nullptr,false)) throw std::runtime_error("Generated native row unavailable");
            started=true;
            auto delivery=deliver(engine_id);
            int remaining=delivery.first;uint8_t destination=delivery.second;
            if (remaining==0) ++accepted;
            log({{"event","runtime_generated_acquisition"},{"base_id",label},{"instance_id",identity},
                {"source",source},{"rarity",record.at("rarity")},
                {"engine_id",record.at("engine_id")},{"affixes",record.at("affixes")},{"implicits",record.at("implicits")},
                {"effective_stats",itemization::effective_stats(record)},{"remaining_count",remaining},{"returned_bag",destination}});
            if (remaining!=0) break;
        }
        return count-accepted;
    } catch (const std::exception& error) {log({{"event","runtime_acquisition_failed"},{"source",source},{"reason",error.what()},{"accepted",accepted}});}
    // A later failure cannot replay and duplicate the whole reward.
    return started?count-accepted:fallback();
}
int runtime_acquire(void* manager,uint64_t item,int count,bool overflow,uint8_t* bag) {
    return generate_and_deliver(manager,item,count,"reward",[&](uint64_t engine_id) {
        uint8_t destination=255;
        int remaining=original_store(manager,engine_id,1,overflow,&destination);
        if (remaining==0 && bag) *bag=destination;
        return std::make_pair(remaining,destination);
    },[&] {return original_store(manager,item,count,overflow,bag);});
}
int shop_delivery_hook(void* manager,int bag,const uint64_t* item,int count) {
    // The inspected helper is called only by the purchase routine. Transfers and
    // sorting use other bag functions and must never generate a replacement item.
    if (!runtime_enabled.load() || !active_shop_purchase)
        return original_shop_delivery(manager,bag,item,count);
    try {
        uint64_t base_id=read<uint64_t>(reinterpret_cast<uintptr_t>(item));
        return generate_and_deliver(manager,base_id,count,"shop",[&](uint64_t engine_id) {
            int remaining=original_shop_delivery(manager,bag,&engine_id,1);
            if (remaining==0) active_shop_purchase->delivered_ids.push_back(engine_id);
            return std::make_pair(remaining,static_cast<uint8_t>(bag));
        },[&] {return original_shop_delivery(manager,bag,item,count);});
    } catch (const std::exception& error) {
        log({{"event","runtime_shop_delivery_failed"},{"reason",error.what()}});
        return count;
    }
}
int shop_buy_hook(void* accessor,bool update_flags) {
    if (!runtime_enabled.load()) return original_shop_buy(accessor,update_flags);
    ShopPurchase purchase;auto previous=active_shop_purchase;
    active_shop_purchase=&purchase;
    int result=original_shop_buy(accessor,update_flags);
    active_shop_purchase=previous;
    try {
        auto address=reinterpret_cast<uintptr_t>(accessor);
        auto base_id=fname(read<uint64_t>(address+0x420));
        auto requested=read<int32_t>(address+0x428);
        // Keep the base ID intact while the original routine calculates price,
        // flags and fallback bag delivery. Afterwards the purchase's action ID
        // must identify the actual rolled item for details and "equip now".
        if (purchase.delivered_ids.size()==1 && requested==1)
            *reinterpret_cast<uint64_t*>(address+0x420)=purchase.delivered_ids.front();
        Json delivered=Json::array();for (auto id:purchase.delivered_ids) delivered.push_back(fname(id));
        log({{"event","runtime_shop_purchase"},{"base_id",base_id},{"requested_count",requested},
            {"delivered_ids",delivered},{"native_result",result},
            {"native_price",read<int32_t>(address+0x464)},{"save_written",false}});
    } catch (const std::exception& error) {log({{"event","runtime_shop_result_failed"},{"reason",error.what()}});}
    return result;
}
void maybe_runtime_test(void* character) noexcept {
    if (!runtime_enabled.load() || granting.load()) return;
    try {
        if (!std::filesystem::exists(runtime_request)) return;
        if (fname(read<uint64_t>(reinterpret_cast<uintptr_t>(character)+0x38))!="UN90000") return;
        // A getter also runs while loading, before saved equipment/bags replace
        // their temporary empty state. Leave the request pending until the
        // caller-specified saved item is equipped AND owned in restored bags.
        {
            std::ifstream pending(runtime_request);const auto preview=Json::parse(pending);
            if(preview.at("action")=="acquire_critical_test_kit") {
                const auto anchor=preview.at("requires_owned_engine_id").get<std::string>();
                std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
                if(!item_service->contains(anchor)) throw std::runtime_error("Critical kit load checkpoint is unknown");
                const auto equipment=read<Array>(reinterpret_cast<uintptr_t>(character)+0x5D8);
                if(equipment.count<0 || equipment.count>8 || equipment.capacity<equipment.count) return;
                const auto anchor_id=native_name(anchor);bool found=false;
                for(int i=0;i<equipment.count;++i) found|=read<uint64_t>(equipment.data+uintptr_t(i)*16)==anchor_id;
                if(!found || owned_count(find_manager(),anchor_id)!=1) return;
            }
        }
        if (granting.exchange(true)) return;
        auto consumed=runtime_request;consumed+=L".consumed";
        if (std::filesystem::exists(consumed)) std::filesystem::remove(consumed);
        std::filesystem::rename(runtime_request,consumed);
        std::ifstream stream(consumed);auto request=Json::parse(stream);
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto manager=find_manager();
        if (request.at("action")=="acquire") {
            if (request.at("base_id")!="E_SHI_0001" || request.at("count")!=2)
                throw std::runtime_error("Controlled runtime acquisition test requires two Pot Lids");
            uint8_t bag=255;int remaining=store_hook(reinterpret_cast<void*>(manager),native_name("E_SHI_0001"),2,false,&bag);
            log({{"event","runtime_acquisition_test"},{"remaining_count",remaining},{"save_written",false}});
        } else if (request.at("action")=="acquire_rarity_test") {
            if (request.at("base_id")!="E_SHI_0001" || request.at("count")!=2)
                throw std::runtime_error("Controlled rarity test requires one Magic and one Rare Pot Lid");
            Json results=Json::array();
            for (const std::string rarity:{"magic","rare"}) {
                int remaining=generate_and_deliver(reinterpret_cast<void*>(manager),native_name("E_SHI_0001"),1,
                    "development_rarity_test",[&](uint64_t engine_id) {
                        if (owned_count(manager,engine_id)!=0) throw std::runtime_error("Fresh test ID is already owned");
                        uint8_t bag=255;
                        int unaccepted=original_store(reinterpret_cast<void*>(manager),engine_id,1,false,&bag);
                        if (unaccepted==0 && owned_count(manager,engine_id)!=1)
                            throw std::runtime_error("Test item delivery did not produce unique ownership");
                        return std::make_pair(unaccepted,bag);
                    },[] {return 1;},rarity);
                results.push_back({{"rarity",rarity},{"remaining_count",remaining}});
                if (remaining!=0) break;
            }
            log({{"event","runtime_rarity_acquisition_test"},{"results",results},{"save_written",false}});
        } else if (request.at("action")=="acquire_critical_test_kit") {
            const std::vector<std::string> expected{"E_CLO_0052","E_HEL_0024","A_ARM_0007","A_ARM_0007"};
            if(request.at("base_ids").get<std::vector<std::string>>()!=expected)
                throw std::runtime_error("Critical test kit requires the four audited Hero bases");
            auto table=read<uintptr_t>(manager+0x38);
            std::vector<Json> planned;
            for(const auto& base_id:expected) {
                const auto& definition=runtime_bases.at(base_id);
                if(!original_item_row(reinterpret_cast<void*>(table),native_name(definition.at("native_base_id")),nullptr,false))
                    throw std::runtime_error("Critical test base is not loaded");
                planned.push_back(item_service->generate_critical_test_item(base_id,runtime_random->below(UINT64_MAX),new_logical_id()));
            }
            // History is durable before each unique ID can enter the inventory.
            // A partial failure stops here; the consumed request is never replayed.
            Json results=Json::array();
            for(const auto& item:planned) {
                auto engine_id=record_runtime_item(item);
                if(!runtime_row(reinterpret_cast<void*>(table),engine_id,nullptr,false) || owned_count(manager,engine_id)!=0)
                    throw std::runtime_error("Critical test row or ownership invalid");
                uint8_t bag=255;
                int remaining=original_store(reinterpret_cast<void*>(manager),engine_id,1,false,&bag);
                results.push_back({{"base_id",item.at("base_id")},{"engine_id",item.at("engine_id")},
                    {"name",itemization::instance_name(item)},{"affixes",item.at("affixes")},{"remaining_count",remaining},{"bag",bag}});
                log({{"event","runtime_critical_test_delivery"},{"item",results.back()},{"save_written",false}});
                if(remaining!=0 || owned_count(manager,engine_id)!=1)
                    throw std::runtime_error("Critical test delivery did not produce unique ownership");
            }
            log({{"event","runtime_critical_test_kit"},{"results",results},{"critical_chance_cap",runtime_total_caps.at("critical_chance")},{"save_written",false}});
        } else {
            std::string old_id=request.at("engine_id");const auto& old_item=item_service->lookup(old_id);
            uint64_t old_engine=native_name(old_id);
            auto equipped=read<Array>(reinterpret_cast<uintptr_t>(character)+0x5D8);
            for (int i=0;i<equipped.count;++i)
                if (read<uint64_t>(equipped.data+uintptr_t(i)*16)==old_engine)
                    throw std::runtime_error("Development edits require an unequipped item");
            if (owned_count(manager,old_engine)!=1) throw std::runtime_error("Edited item must exist exactly once");
            uintptr_t payload=0;
            for (int bag=0;bag<=12;++bag) {
                auto array=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
                for (int i=0;i<array.count;++i) {
                    auto candidate=read<uintptr_t>(array.data+uintptr_t(i)*16);
                    if (read<uint64_t>(candidate+0x10)==old_engine) payload=candidate;
                }
            }
            if (!payload) throw std::runtime_error("Cannot locate edited instance");
            std::string action=request.at("action");Json changed;
            if (action=="remove_affix") changed=item_service->remove_affix(old_id,request.at("affix_id"));
            else if (action=="add_affix") changed=item_service->add_affix(old_id,request.at("affix_id"),runtime_random->below(UINT64_MAX));
            else if (action=="reroll_affix") changed=item_service->reroll_affix(old_id,request.at("affix_id"),runtime_random->below(UINT64_MAX));
            else if (action=="upgrade_rarity") changed=item_service->upgrade_rarity(old_id,request.at("rarity"));
            else throw std::runtime_error("Unsupported development edit");
            auto new_engine=record_runtime_item(changed);
            auto table=read<uintptr_t>(manager+0x38);
            if (!runtime_row(reinterpret_cast<void*>(table),new_engine,nullptr,false)) throw std::runtime_error("Edited row unavailable");
            // Single aligned engine-ID update, on the game's own getter caller.
            // Logical identity stays fixed; old revisions remain readable for old saves.
            *reinterpret_cast<uint64_t*>(payload+0x10)=new_engine;
            log({{"event","runtime_item_edited"},{"instance_id",changed.at("instance_id")},
                {"old_engine_id",old_item.at("engine_id")},{"new_engine_id",changed.at("engine_id")},
                {"action",action},{"affixes",changed.at("affixes")},{"effective_stats",itemization::effective_stats(changed)},
                {"save_written",false}});
        }
    } catch (const std::exception& error) {log({{"event","runtime_test_refused_or_stopped"},{"reason",error.what()}});}
    catch (...) {log({{"event","runtime_test_failed"}});}
    granting=false;
}
Json veyl_wallet(uintptr_t manager);
Json veyl_commit(uintptr_t accessor,uint64_t source,const itemization::VeylChoice& choice,int quoted_cost);
Json veyl_recipe_commit(uintptr_t accessor,uint64_t recipe);
Json shard_wallet(uintptr_t manager);
Json shard_commit(uintptr_t accessor,uint64_t source,const std::string& family);
std::vector<Json> salvage_inventory(uintptr_t manager);
bool salvage_source_valid(uintptr_t manager,uint64_t id);
Json salvage_commit(const std::vector<std::string>& sources);
void camp_wallet_update(uintptr_t accessor) noexcept;
#include "forge_probe.hpp"
#include "veyl_bridge.hpp"
#include "camp_crafting_bridge.hpp"
#include "craft_inventory_bridge.hpp"
bool initialize_runtime(const std::filesystem::path& folder) {
    std::ifstream config(folder/L"runtime-config.json");if (!config) return false;
    auto settings=Json::parse(config);
    forge_trace_enabled=settings.value("forge_trace",false);
    forge_modifiers_enabled=settings.value("forge_modifiers",false);
    if (forge_modifiers_enabled) {
        std::ifstream forge_file(folder/L"forge-modifiers.json");
        if (!forge_file) throw std::runtime_error("Missing forge modifier policy");
        forge_policy=std::make_unique<itemization::ForgePolicy>(itemization::ForgePolicy::parse(Json::parse(forge_file)));
    }
    if (settings.at("schema_version")!=1 || settings.at("steam_build")!=6291077 ||
        !settings.at("runtime_generation").get<bool>()) throw std::runtime_error("Unsupported runtime configuration");
    std::ifstream bases_file(folder/L"item-bases.json"),affixes_file(folder/L"affixes.json");
    auto bases_json=Json::parse(bases_file);
    std::ifstream custom_bases(folder/L"custom-bases.json");
    if (custom_bases) {
        auto custom=Json::parse(custom_bases);
        if (custom.at("schema_version")!=1 || !custom.at("bases").is_array()) throw std::runtime_error("Invalid custom base catalog");
        for (const auto& definition:custom.at("bases")) bases_json["bases"].push_back(definition);
    }
    bases_json=itemization::BaseRegistry::resolve(bases_json);
    auto bases=itemization::BaseRegistry::parse(bases_json);
    for (const auto& definition:bases_json.at("bases")) runtime_bases.emplace(definition.at("base_id").get<std::string>(),definition);
    auto affixes_json=Json::parse(affixes_file);
    runtime_total_caps=affixes_json.value("total_caps",Json::object());
    runtime_rarity_weights=settings.value("reward_rarity_weights",Json::object());
    if(!runtime_rarity_weights.empty()) {
        uint64_t total=0;for(auto i=runtime_rarity_weights.begin();i!=runtime_rarity_weights.end();++i) {
            if(itemization::lifetime_rules().count(i.key())==0 || !i.value().is_number_integer() || i.value().get<int64_t>()<0 || i.value().get<int64_t>()>1000000)
                throw std::runtime_error("Invalid reward rarity weights");
            total+=i.value().get<uint32_t>();
        }
        if(total==0) throw std::runtime_error("Empty reward rarity pool");
    }
    item_service=std::make_unique<itemization::ItemService>(std::move(bases),itemization::Catalog::parse(affixes_json));
    history_path=folder/L"state"/L"item-history.json";runtime_request=folder/L"lifetime-test.request";
    std::ifstream history(history_path);if (history) item_service->import_history(Json::parse(history));
    auto loaded=item_service->export_history();
    for (const auto& item:loaded.at("revisions")) {
        std::string id=item.at("engine_id");runtime_ids.emplace(native_name(id),id);
    }
    std::random_device entropy;
    runtime_random=std::make_unique<itemization::Random>((uint64_t(entropy())<<32)^entropy()^GetTickCount64());
    if(settings.value("veyl_crafting",false)) {
        if(!forge_modifiers_enabled) throw std::runtime_error("Veyl entry point requires crafting UI hooks");
        validate_veyl_bindings();
        std::ifstream currencies(folder/L"veyls.json");if(!currencies) throw std::runtime_error("Missing Veyl policy");
        veyl_policy=std::make_unique<itemization::VeylPolicy>(Json::parse(currencies));
        for(const auto& c:veyl_policy->currencies()) veyl_native_ids.emplace(c.at("id"),native_name(c.at("native_id")));
        if(settings.value("camp_crafting",false)) {
            std::ifstream camp(folder/L"camp-crafting.json");if(!camp) throw std::runtime_error("Missing campsite crafting policy");
            camp_policy=std::make_unique<itemization::CampCraftPolicy>(Json::parse(camp));
            for(const auto& s:camp_policy->shards()) shard_native_ids.emplace(s.at("family_id"),native_literal_name(s.at("native_id")));
            log({{"event","camp_policy_loaded"},{"shard_families",shard_native_ids.size()},{"quality_maximum_bp",2500},{"salvage_base_chance_bp",camp_policy->base_chance()},{"salvage_bonus_chance_bp",camp_policy->bonus_chance()}});
        }
        log({{"event","veyl_policy_loaded"},{"currencies",veyl_native_ids.size()},{"wallet","native adventure-log inventory"}});
    }
    log({{"event","runtime_config_loaded"},{"bases",runtime_bases.size()},{"saved_revisions",loaded.at("revisions").size()},
        {"generation","at acquisition"},{"state_file",history_path.string()}});
    return true;
}
