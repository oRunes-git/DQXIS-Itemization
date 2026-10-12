// Included after the runtime item service. Only pure result multipliers are live.
bool combat_scalars_enabled=false;
Json native_scalar_actions=Json::object();
Json native_scalar_damage_rows=Json::object();
itemization::CombatBalance native_scalar_balance;
std::atomic<uint64_t> scalar_log_count{0};
void initialize_combat_scalars(const std::filesystem::path& folder,const Json& settings) {
    combat_scalars_enabled=settings.value("native_combat_scalars",false);
    if(!combat_scalars_enabled) return;
    std::ifstream actions_file(folder/L"action-affix-tags.json"),catalog_file(folder/L"affixes.json");
    if(!actions_file || !catalog_file) throw std::runtime_error("Missing native scalar action catalog");
    auto tags=Json::parse(actions_file);if(tags.at("schema_version")!=1 || tags.value("classification_version",0)!=2 ||
        !tags.at("actions").is_object() || !tags.at("damage_rows").is_object())
        throw std::runtime_error("Invalid native scalar action catalog");
    native_scalar_actions=tags.at("actions");native_scalar_damage_rows=tags.at("damage_rows");
    native_scalar_balance=itemization::CombatBalance::parse(Json::parse(catalog_file));
    log({{"event","combat_scalar_adapter_loaded"},{"effects",Json::array({"spell_damage","healing_power","critical_damage"})},
        {"pure_result_multiplier",true},{"gameplay_verified",false}});
}
std::vector<Json> generated_combat_equipment(uintptr_t character) {
    std::vector<Json> result;if(!character || !manager_object_is_a(character,"JackFriendGameCharacter")) return result;
    const auto items=read<Array>(character+0x5D8);
    if(items.count<0 || items.count>32 || items.capacity<items.count) throw std::runtime_error("Invalid combat equipment array");
    for(int i=0;i<items.count;++i) {
        const auto id=read<uint64_t>(items.data+uintptr_t(i)*16);auto found=runtime_ids.find(id);
        if(found!=runtime_ids.end()) result.push_back(item_service->lookup(found->second));
    }
    return result;
}
Json apply_native_combat_scalar(void* output,void* attacker,void* context) noexcept {
    if(!runtime_enabled.load() || !combat_scalars_enabled) return Json();
    try {
        const uintptr_t damage=reinterpret_cast<uintptr_t>(output),source=reinterpret_cast<uintptr_t>(attacker);
        const auto action_name=fname(read<uint64_t>(damage));
        const auto override_row=context?fname(read<uint64_t>(reinterpret_cast<uintptr_t>(context)+0x20)):"None";
        const auto action=itemization::resolve_native_action_tags(native_scalar_actions,native_scalar_damage_rows,action_name,override_row);
        if(!action.is_object()) return Json();
        const auto damage_type=read<uint8_t>(damage+0x18);
        const auto category=action.value("action_category",std::string{});
        const bool critical=read<uint8_t>(damage+0x40)!=0;
        const bool physical_critical=damage_type==1 && critical && action.value("physical",false) && !action.value("spell",false) &&
            (category=="EJackActionCategory__Kougeki" || category=="EJackActionCategory__Tokugi");
        if(!(physical_critical || (damage_type==1 && category=="EJackActionCategory__Jumon") ||
            (damage_type==3 && (category=="EJackActionCategory__Jumon" || category=="EJackActionCategory__Tokugi")))) return Json();
        // Preserve all native guards, reflection, no-parameter-change and
        // zero-resource exceptions; scaling never creates a blocked hit.
        bool blocked=false;for(const size_t offset:{0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x51,0x52,0x53})
            blocked=blocked || read<uint8_t>(damage+offset)!=0;
        itemization::CombatAffixes effects;std::string origin_weapon;
        const bool metal=context && read<uint8_t>(reinterpret_cast<uintptr_t>(context)+0xD)!=0;
        {std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            if(physical_critical) {
                // A unique main-hand weapon gives an unambiguous source for
                // normal attacks and weapon skills. Shields are not weapons.
                // Dual-wield/unarmed/unknown origins await their own adapter.
                if(!context || metal || !source) return Json();
                const auto items=read<Array>(source+0x5D8);
                if(items.count<0 || items.count>32 || items.capacity<items.count) return Json();
                std::vector<itemization::NativeWeaponSlot> hands;
                for(int i=0;i<items.count;++i) {
                    const auto slot=items.data+uintptr_t(i)*16;const auto id=read<uint64_t>(slot);const auto region=read<uint8_t>(slot+8);
                    if((region!=1 && region!=2) || fname(id)=="None") continue;
                    const auto found=runtime_ids.find(id);std::string item_category;
                    if(found!=runtime_ids.end()) item_category=item_service->lookup(found->second).at("category");
                    else {const auto vanilla=runtime_bases.find(fname(id));if(vanilla==runtime_bases.end()) return Json();item_category=vanilla->second.at("category");}
                    hands.push_back({found!=runtime_ids.end()?found->second:fname(id),item_category,region});
                }
                const auto origin=itemization::native_critical_origin(hands);
                if(!origin.valid) return Json();origin_weapon=origin.weapon;
            }
            effects=itemization::CombatAffixes::collect(generated_combat_equipment(source),origin_weapon);}
        const auto projection=itemization::native_scalar_projection(read<int32_t>(damage+0x1C),damage_type,action,blocked,effects,native_scalar_balance,critical,metal);
        if(projection.stat.empty() || projection.bonus==0) return Json();
        Json details={{"stat",projection.stat},{"bonus",projection.bonus},{"before",projection.before},{"after",projection.after},
            {"limit",projection.limit},{"reason",projection.reason},{"action_category",action.at("action_category")},
            {"damage_row",action.at("damage_row")},{"combat_profile",action.at("combat_profile")},
            {"critical",critical},{"metal",metal},{"origin_weapon",origin_weapon},{"resources_written",false},{"additional_attack",false}};
        if(projection.changed()) {
            SIZE_T written=0;const int32_t amount=projection.after;
            if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(damage+0x1C),&amount,sizeof(amount),&written) || written!=sizeof(amount))
                throw std::runtime_error("Cannot publish native scalar result");
        }
        if(scalar_log_count.fetch_add(1)<250) log({{"event","combat_scalar_result"},{"action",action_name},{"scalar",details},
            {"damage_modified",projection.changed()},{"resources_written",false}});
        return details;
    }catch(const std::exception& error) {if(scalar_log_count.fetch_add(1)<250) log({{"event","combat_scalar_failed"},{"reason",error.what()}});}
    catch(...) {if(scalar_log_count.fetch_add(1)<250) log({{"event","combat_scalar_failed"},{"reason","Unknown error"}});}
    return Json();
}
