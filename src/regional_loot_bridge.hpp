// Included after lifetime_bridge.hpp: use its single item-generation/delivery path.
std::unique_ptr<itemization::RegionalLoot> regional_loot;
std::unique_ptr<itemization::RarityProgression> rarity_progression;
using RewardCalculationFn=void(*)(void*);
RewardCalculationFn original_reward_calculation=nullptr;
using SaveGetterFn=uintptr_t(*)(void*);
using SaveCountFn=int(*)(void*,uint64_t);
using SaveGroupFn=uint8_t(*)(void*);
using DefeatedFn=Array*(*)(void*,Array*);
using DropCopyFn=void*(*)(void*,const void*);
using DropGrowFn=void(*)(Array*,int);
using FlagBoolFn=bool(*)(void*,int,uint64_t);
std::map<std::string,uint64_t> loot_saved_names;
Json loot_monster_records;
std::map<std::string,std::vector<uint64_t>> loot_defeat_groups;
void feedback_require_property(uintptr_t cls,const std::string& property,int offset);
std::set<uintptr_t> loot_validated_classes;
void loot_require_fields(uintptr_t object,std::initializer_list<std::pair<const char*,int>> fields) {
    manager_handle(object);
    const auto cls=read<uintptr_t>(object+0x10);
    if(loot_validated_classes.count(cls)) return;
    for(const auto& f:fields) feedback_require_property(cls,f.first,f.second);
    loot_validated_classes.insert(cls);
}
std::set<std::string> rewarded_battles;
std::vector<std::string> rewarded_battle_order;

uint64_t loot_native_wide_name(const std::string& s) {
    const auto text=wide_utf8(s);
    uint64_t result=0;
    reinterpret_cast<uint64_t*(*)(uint64_t*,const wchar_t*,int)>(base+0xD69850)(&result,text.c_str(),1);
    return result;
}
uintptr_t loot_save(void* context) {
    const auto save=reinterpret_cast<SaveGetterFn>(base+0x7EC6E0)(context);
    if(!save || !manager_object_is_a(save,"JackSaveData")) return 0;
    manager_handle(save);return save;
}
int loot_defeat_count(uintptr_t save,const std::string& zukan) {
    int64_t count=0;
    // SaveData's counter is keyed by native monster row, not bestiary family.
    // Sum all family variants, including the story-only _Sxx monster rows.
    for(const auto id:loot_defeat_groups.at(zukan)) count+=std::max(0,
        reinterpret_cast<SaveCountFn>(base+0x7B1940)(reinterpret_cast<void*>(save),id));
    return static_cast<int>(std::min<int64_t>(count,INT_MAX));
}
int loot_boss_credits(uintptr_t save) {
    return rarity_progression->credit_count([&](const std::string& id) {
        return loot_defeat_count(save,id);
    },[&](const std::string& id) {
        return reinterpret_cast<FlagBoolFn>(base+0x751430)(reinterpret_cast<void*>(save),3,loot_saved_names.at(id));
    });
}
Json acquisition_rarity_weights(void* context) {
    if(rarity_progression) {
        const auto save=loot_save(context);
        if(save) return rarity_progression->weights(loot_boss_credits(save)).json();
    }
    return runtime_rarity_weights;
}
// Owned engine memory, including the owner FString, is constructed/copied by the
// same helpers used in the game's reward routine. No STL buffer enters a TArray.
struct NativeLootEntry {
    uint64_t item=0;Array owner{};uintptr_t owner_character=0;
    int32_t rare_box=0,rarity=0,probability=1,sort_no=0,random=0,padding=0;
};
static_assert(sizeof(NativeLootEntry)==0x38,"Native loot entry ABI");
static_assert(offsetof(NativeLootEntry,rare_box)==0x20,"Native loot fields ABI");
void append_regional_reward(uintptr_t result,const NativeLootEntry& entry) {
    auto* array=reinterpret_cast<Array*>(result+0x60);
    const auto before=read<Array>(result+0x60);
    if(before.count<0 || before.count>128 || before.capacity<before.count || before.capacity>100000 || (before.count && !before.data))
        throw std::runtime_error("Invalid native reward array");
    // Matches 0x4D397D..0x4D39A6: increment count, native grow, copy construct.
    array->count=before.count+1;
    if(array->count>array->capacity) reinterpret_cast<DropGrowFn>(base+0x1A59490)(array,before.count);
    if(!array->data || array->capacity<array->count) {array->count=before.count;throw std::runtime_error("Native loot array grow failed");}
    reinterpret_cast<DropCopyFn>(base+0x4D0A00)(reinterpret_cast<void*>(array->data+uintptr_t(before.count)*0x38),&entry);
}
struct DefeatedLootArray {
    Array value{};
    ~DefeatedLootArray() {if(value.data) reinterpret_cast<void(*)(void*)>(base+0xCBDFB0)(reinterpret_cast<void*>(value.data));}
};
uintptr_t loot_event_battle(uintptr_t battle) {
    const int index=read<int32_t>(battle+0x48C),serial=read<int32_t>(battle+0x490);
    if(index<0 || serial<=0) return 0;
    const int count=read<int32_t>(base+0x5D83BF8+0x1C);
    if(index>=count) return 0;
    const auto slot=read<uintptr_t>(base+0x5D83BF8+0x10)+uintptr_t(index)*24;
    const auto object=read<uintptr_t>(slot);
    return object && read<int32_t>(slot+0x10)==serial && manager_object_is_a(object,"JackEventBattleInfo")?object:0;
}
bool veyl_claimed(uintptr_t save,const std::string& flag) {
    return reinterpret_cast<FlagBoolFn>(base+0x751430)(reinterpret_cast<void*>(save),3,native_name(flag));
}
void veyl_claim(uintptr_t save,const std::string& flag) {
    reinterpret_cast<void(*)(void*,int,uint64_t,bool)>(base+0x759EA0)(reinterpret_cast<void*>(save),3,native_name(flag),true);
    if(!veyl_claimed(save,flag)) throw std::runtime_error("Veyl claim write refused: "+flag);
}
void veyl_migrate_claims(uintptr_t save,const std::string& current_event) {
    if(!veyl_policy || veyl_claimed(save,veyl_policy->config().at("migration_flag"))) return;
    for(const auto& b:rarity_progression->bosses()) {
        if(b.at("event_id")==current_event) continue;
        bool done=true;
        for(const auto& r:b.at("requirements")) {
            if(r.contains("zukan_id")) done=done && loot_defeat_count(save,r.at("zukan_id"))>=r.at("count").get<int>();
            else done=done && reinterpret_cast<FlagBoolFn>(base+0x751430)(reinterpret_cast<void*>(save),3,loot_saved_names.at(r.at("scenario_flag")));
        }
        if(done) for(const auto& e:veyl_policy->config().at("encounters")) if(e.at("event_id")==b.at("event_id")) veyl_claim(save,e.at("claim_flag"));
    }
    veyl_claim(save,veyl_policy->config().at("migration_flag"));
}
void veyl_story_reward(uintptr_t battle,uintptr_t save,const itemization::RegionalPool* pool) {
    if(!veyl_policy) return;
    const auto event=loot_event_battle(battle);if(!event) return;
    loot_require_fields(event,{{"EventBattleID",0x38}});
    const auto id=fname(read<uint64_t>(event+0x38));
    veyl_migrate_claims(save,id);
    for(const auto& e:veyl_policy->config().at("encounters")) {
        if(e.at("event_id")!=id || veyl_claimed(save,e.at("claim_flag"))) continue;
        const int cls=pool?pool->class_ceiling:1;
        const auto bundle=veyl_policy->encounter_bundle(e.at("kind")=="boss",cls);
        const auto manager=find_manager();if(!manager) throw std::runtime_error("Story currency inventory unavailable");
        const auto before=veyl_wallet(manager);
        // Boss scenes can skip the result window. Native inventory delivery
        // handles both cases without depending on the presentation of rewards.
        try {
            for(auto q=bundle.begin();q!=bundle.end();++q) {
                const int desired=std::min(9999,before.at(q.key()).get<int>()+q.value().get<int>());
                veyl_set_quantity(manager,veyl_native_ids.at(q.key()),desired);
            }
            veyl_claim(save,e.at("claim_flag"));
        } catch(...) {
            for(auto q=bundle.begin();q!=bundle.end();++q) veyl_set_quantity(manager,veyl_native_ids.at(q.key()),before.at(q.key()));
            throw;
        }
        log({{"event","veyl_story_reward"},{"encounter",id},{"kind",e.at("kind")},{"bundle",bundle},{"region_class",cls},{"saved_claim",true}});
        return;
    }
}
void reward_calculation_hook(void* sequence) {
    original_reward_calculation(sequence);
    if(!runtime_enabled.load() || !regional_loot) return;
    Json context=Json::object();
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        const auto battle=read<uintptr_t>(reinterpret_cast<uintptr_t>(sequence)+0x10);
        if(!manager_object_is_a(battle,"JackBattleManager")) return;
        loot_require_fields(battle,{{"BattleStatus",0x4C0},{"BattleResultInfo",0x4C8},{"BattleCharacterManager",0x4D0},{"Encount_EventBattleInfo",0x48C}});
        const auto status=read<uintptr_t>(battle+0x4C0),result=read<uintptr_t>(battle+0x4C8),characters=read<uintptr_t>(battle+0x4D0);
        if(!manager_object_is_a(status,"JackBattleStatus") || !manager_object_is_a(result,"JackBattleResultInfo") ||
            !manager_object_is_a(characters,"JackBattleCharacterManager")) return;
        loot_require_fields(status,{{"BattleResultType",0x51},{"bSkipResult",0x4A}});
        loot_require_fields(result,{{"BattleResultType",0x38},{"DropItem",0x60}});
        if(read<uint8_t>(status+0x51)!=1 || read<uint8_t>(result+0x38)!=1) return;
        const auto save=loot_save(reinterpret_cast<void*>(battle));if(!save) return;
        const std::string map=fname(read<uint64_t>(save+0x318));
        context["map"]=map;
        const uint8_t group=reinterpret_cast<SaveGroupFn>(base+0x7B0EA0)(reinterpret_cast<void*>(save));
        if(group<1 || group>4) {log({{"event","regional_loot_unresolved_state"},{"map",map},{"group",group}});return;}
        const std::string phase(1,"ABCS"[group-1]);const auto* pool=regional_loot->find(map,phase);
        context["scenario"]=phase;
        if(loot_event_battle(battle)) {veyl_story_reward(battle,save,pool);return;}
        if(read<uint8_t>(status+0x4A)) return;
        if(!pool) {log({{"event","regional_loot_unresolved_region"},{"map",map},{"scenario",phase}});return;}
        DefeatedLootArray defeated;
        reinterpret_cast<DefeatedFn>(base+0x4B9CA0)(reinterpret_cast<void*>(characters),&defeated.value);
        if(defeated.value.count<0 || defeated.value.count>100 || defeated.value.capacity<defeated.value.count)
            throw std::runtime_error("Invalid defeated-monster array");
        std::vector<std::pair<uintptr_t,std::string>> eligible;
        Json defeated_ids=Json::array(),ineligible_ids=Json::array();
        std::vector<std::string> identities;std::set<uintptr_t> unique_characters;
        std::set<std::string> defeated_zukan_ids;
        const auto result_handle=manager_handle(result);
        for(int i=0;i<defeated.value.count;++i) {
            const auto character=read<uintptr_t>(defeated.value.data+uintptr_t(i)*8);
            if(!manager_object_is_a(character,"JackMonsterGameCharacter")) continue;
            if(!unique_characters.insert(character).second) continue;
            const auto handle=manager_handle(character);
            identities.push_back(std::to_string(handle.index)+":"+std::to_string(handle.serial));
            // MonsterID is private/unreflected. Use the guarded native getter,
            // after validating the character's type and lifetime above.
            const auto id=fname(itemization::defeated_monster_name(
                reinterpret_cast<itemization::MonsterNameGetter>(base+0x78F2C0),reinterpret_cast<void*>(character)));
            defeated_ids.push_back(id);
            if(pool->monsters.count(id)) {
                eligible.emplace_back(character,id);
                const std::string zukan=loot_monster_records.at(id).at("zukan_id");
                if(zukan!="None") defeated_zukan_ids.insert(zukan);
            } else ineligible_ids.push_back(id);
        }
        context["defeated_monsters"]=defeated_ids;
        std::sort(identities.begin(),identities.end());
        std::string key=map+":"+phase+":"+std::to_string(result_handle.index)+":"+std::to_string(result_handle.serial)+":";
        for(const auto& id:identities) key+=id+",";
        // Counts distinguish later battles if the engine reuses a monster object.
        for(const auto& id:defeated_zukan_ids) key+=id+":"+std::to_string(loot_defeat_count(save,id))+",";
        if(!rewarded_battles.insert(key).second) return;
        rewarded_battle_order.push_back(key);
        if(rewarded_battle_order.size()>256) {rewarded_battles.erase(rewarded_battle_order.front());rewarded_battle_order.erase(rewarded_battle_order.begin());}
        if(eligible.empty()) {
            log({{"event","regional_loot_no_eligible_monsters"},{"region",pool->id},{"context",context}});return;
        }
        const int credits=loot_boss_credits(save);const auto weights=rarity_progression->weights(credits);
        Json currency_drops=Json::object();
        // Claim migration only controls one-time story bundles. An error there
        // must not prevent the independent regular-victory currency rolls.
        if(veyl_policy) try {veyl_migrate_claims(save,"");}
        catch(const std::exception& e) {log({{"event","veyl_claim_migration_failed"},{"reason",e.what()},{"context",context}});}
        try {if(veyl_policy) {
            const auto rolled=veyl_policy->roll_drops(*runtime_random,credits,pool->class_ceiling);
            for(auto q=rolled.begin();q!=rolled.end();++q) for(int i=0;i<q.value().get<int>();++i) {
                NativeLootEntry entry;entry.item=veyl_native_ids.at(q.key());entry.owner_character=eligible.front().first;
                entry.sort_no=9100;entry.rarity=1;
                DefeatedLootArray owner;runtime_text(&owner.value,loot_monster_records.at(eligible.front().second).at("name"));entry.owner=owner.value;
                append_regional_reward(result,entry);
                currency_drops[q.key()]=currency_drops.value(q.key(),0)+1;
            }
        }} catch(const std::exception& e) {
            // A currency archive/claim failure must not suppress equipment rolls.
            log({{"event","regional_currency_failed"},{"reason",e.what()},{"context",context}});
        }
        const auto manager=find_manager();if(!manager) throw std::runtime_error("Item manager unavailable for regional drop");
        int granted=0;
        Json equipment_drops=Json::array(),equipment_rolls=Json::array();
        for(const auto& monster:eligible) {
            if(granted>=regional_loot->battle_limit()) break;
            const auto roll=regional_loot->roll_equipment(*runtime_random);
            equipment_rolls.push_back({{"monster_id",monster.second},{"draw",roll.draw},{"threshold",roll.threshold},
                {"succeeded",roll.succeeded()}});
            if(!roll.succeeded()) continue;
            const auto base_id=regional_loot->pick_base(*pool,*runtime_random);
            const auto rarity=weights.roll(*runtime_random);
            const auto remaining=generate_and_deliver(reinterpret_cast<void*>(manager),native_name(base_id),1,"monster_drop",
                [&](uint64_t engine_id) {
                    NativeLootEntry entry;entry.item=engine_id;entry.owner_character=monster.first;
                    // Use the base's native reward ordering/box classification. Mod rarity lives on its instance.
                    const auto& fields=runtime_bases.at(base_id).at("native_fields");
                    entry.rarity=std::stoi(fields.at("Rarity").get<std::string>());
                    entry.sort_no=std::stoi(fields.at("SortNo").get<std::string>());
                    DefeatedLootArray owner;
                    runtime_text(&owner.value,loot_monster_records.at(monster.second).at("name"));entry.owner=owner.value;
                    append_regional_reward(result,entry);return std::make_pair(0,uint8_t(255));
                },[]{return 1;},rarity);
            if(remaining==0) {++granted;equipment_drops.push_back({{"base_id",base_id},{"rarity",rarity},{"monster_id",monster.second}});}
            equipment_rolls.back()["queued"]=remaining==0;
        }
        log({{"event","regional_loot_battle"},{"region",pool->id},{"boss_credits",credits},{"rarity_weights_bp",weights.json()},
            {"context",context},{"ineligible_monsters",ineligible_ids},{"equipment_chance_bp",regional_loot->chance_bp()},
            {"eligible_monsters",eligible.size()},{"bonus_equipment",granted},{"equipment",equipment_drops},{"equipment_rolls",equipment_rolls},
            {"veyls",currency_drops},{"vanilla_loot_preserved",true}});
    } catch(const std::exception& e) {log({{"event","regional_loot_failed"},{"reason",e.what()},{"context",context}});}
}
void initialize_regional_loot(const std::filesystem::path& folder,const Json& settings) {
    if(!settings.value("regional_loot",false)) return;
    // Entry/ABI guards plus instruction guards proving all field displacements.
    const std::vector<std::pair<uintptr_t,std::vector<uint8_t>>> guards{
        {0x4D73A0,{0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x55}},
        {0x4D0A00,{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10}},
        {0x1A59490,{0x48,0x89,0x5C,0x24,0x08,0x57}},
        {0x4B9CA0,{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18}},
        {0x7B1940,{0x40,0x57,0x8B,0x81,0x48,0x03,0x00,0x00}},
        {0x7B0EA0,{0x40,0x57,0x48,0x83,0xEC,0x20,0x48,0x8D,0x79,0x38}},
        {0x7EC6E0,{0x48,0x8B,0x05,0x29,0xBB,0x41,0x05}},
        {0xCBDFB0,{0x48,0x85,0xC9,0x74,0x1D}},
        {0xD69850,{0x40,0x53,0x48,0x83,0xEC,0x30}},
        {0x751430,{0x4C,0x89,0x44,0x24,0x18,0x53}},
        {0x7B0DF0,{0x48,0x8B,0x81,0x18,0x03,0x00,0x00}},
        {0x78F2C0,{0x48,0x8B,0x81,0x50,0x06,0x00,0x00,0x48,0x89,0x02,0x48,0x8B,0xC2,0xC3}},
        {0x4D7B01,{0x49,0x8D,0x55,0x58,0x48,0x8B,0x0B,0x48,0x83,0xC1,0x60}},
        {0x4D397D,{0x4C,0x63,0x73,0x60,0x41,0x8D,0x46,0x01,0x89,0x43,0x60}}
    };
    for(const auto& g:guards) for(size_t i=0;i<g.second.size();++i)
        if(read<uint8_t>(base+g.first+i)!=g.second[i]) throw std::runtime_error("Regional loot native binding mismatch");
    std::ifstream pool_file(folder/L"regional-loot.json"),rarity_file(folder/L"rarity-progression.json");
    if(!pool_file || !rarity_file) throw std::runtime_error("Regional loot catalog missing");
    std::ifstream base_file(folder/L"item-bases.json");
    auto catalog=Json::parse(pool_file);loot_monster_records=catalog.at("monster_records");
    regional_loot=std::make_unique<itemization::RegionalLoot>(catalog,itemization::BaseRegistry::parse(Json::parse(base_file)));
    const auto progression=Json::parse(rarity_file);
    rarity_progression=std::make_unique<itemization::RarityProgression>(progression);
    for(auto g=progression.at("defeat_record_groups").begin();g!=progression.at("defeat_record_groups").end();++g) {
        std::set<uint64_t> ids;
        for(const auto& member:g.value()) ids.insert(native_name(member.get<std::string>()));
        if(ids.empty()) throw std::runtime_error("Empty native defeat record group");
        loot_defeat_groups[g.key()]={ids.begin(),ids.end()};
    }
    for(const auto& b:rarity_progression->bosses()) for(const auto& r:b.at("requirements")) {
        if(r.contains("zukan_id")) {const std::string id=r.at("zukan_id");if(!loot_defeat_groups.count(id)) throw std::runtime_error("Unmapped story boss defeat counter");}
        else {const std::string id=r.at("scenario_flag");loot_saved_names[id]=loot_native_wide_name(id);}
    }
    for(auto r=loot_monster_records.begin();r!=loot_monster_records.end();++r) {
        const std::string id=r.value().at("zukan_id");if(id!="None" && !loot_defeat_groups.count(id)) throw std::runtime_error("Unmapped regular monster defeat counter");
    }
    log({{"event","regional_loot_loaded"},{"regions",regional_loot->regions().size()},{"bosses",rarity_progression->bosses().size()},
        {"live_reward_delivery_verified",false}});
}
void create_regional_loot_hook() {
    if(regional_loot && MH_CreateHook(reinterpret_cast<void*>(base+0x4D73A0),reinterpret_cast<void*>(&reward_calculation_hook),
        reinterpret_cast<void**>(&original_reward_calculation))!=MH_OK) throw std::runtime_error("Regional loot hook creation failed");
}
void enable_regional_loot_hook() {
    if(regional_loot && MH_EnableHook(reinterpret_cast<void*>(base+0x4D73A0))!=MH_OK) throw std::runtime_error("Regional loot hook activation failed");
}
