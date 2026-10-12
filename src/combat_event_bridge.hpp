// Effects are delivered only at the engine's committed action-module boundary.
// Damage previews never enter this bridge. Native setters retain HP/MP limits,
// death rules, UI notifications and native status lifetime/overwrite handling.
bool combat_events_enabled=false;
std::recursive_mutex combat_event_mutex;
itemization::AffixEventExecutor combat_event_executor;
itemization::Random combat_event_random{std::random_device{}()};
uint64_t combat_battle_generation=1,combat_action_sequence=0,combat_hit_sequence=0;
uintptr_t combat_event_manager=0;
struct NativeActionEvents {uint64_t root=0,exec=0;bool momentum=false;};
std::map<uintptr_t,NativeActionEvents> combat_actor_actions;
std::map<uintptr_t,uint64_t> combat_actor_turns;
std::map<uintptr_t,uint64_t> combat_momentum_expiry;
std::set<uint64_t> combat_started_execs;
std::map<std::string,uint64_t> combat_native_statuses;
bool dispatch_momentum_active(uintptr_t actor) noexcept {
    try {std::lock_guard<std::recursive_mutex> lock(combat_event_mutex);
        auto action=combat_actor_actions.find(actor);
        if(action!=combat_actor_actions.end() && action->second.momentum) return true;
        auto armed=combat_momentum_expiry.find(actor);
        return armed!=combat_momentum_expiry.end() && combat_actor_turns[actor]<=armed->second;
    } catch(...) {return false;}
}
void combat_reset_events() {
    ++combat_battle_generation;combat_event_executor.reset(combat_battle_generation);
    combat_actor_actions.clear();combat_actor_turns.clear();combat_momentum_expiry.clear();combat_started_execs.clear();
}
void initialize_combat_events(const Json& settings) {
    combat_events_enabled=combat_dispatch_enabled && settings.value("native_combat_events",false);
    if(!combat_events_enabled) return;
    combat_event_executor=itemization::AffixEventExecutor(native_scalar_balance);
    combat_native_statuses={{"poison",native_name("Doku")},{"sleep",native_name("Nemuri")},{"defence_down",native_name("ODD_DEF_DOWN1")}};
    log({{"event","combat_event_adapter_loaded"},{"delivery","committed native modules and wearer turns"},{"gameplay_verified",false}});
}
void* combat_weak_actor(uintptr_t handle) {return reinterpret_cast<void*(*)(void*)>(base+0xF1B7C0)(reinterpret_cast<void*>(handle));}
uint64_t combat_weak_id(void* actor) {
    uint64_t weak=0;reinterpret_cast<void(*)(void*,void*)>(base+0xF1B650)(&weak,actor);return weak;
}
itemization::AffixResources combat_resources(void* actor) {
    const auto address=reinterpret_cast<uintptr_t>(actor);
    return {read<int>(address+0x7C),read<int>(address+0x80),reinterpret_cast<int(*)(void*)>(base+0x716FB0)(actor),reinterpret_cast<int(*)(void*)>(base+0x717000)(actor)};
}
void combat_apply_recovery(void* actor,const itemization::AffixEvent& event,const itemization::CombatAffixes& effects) {
    if(!actor || !manager_object_is_a(reinterpret_cast<uintptr_t>(actor),"JackFriendGameCharacter")) return;
    const auto resources=combat_resources(actor);
    const auto recovery=combat_event_executor.recovery(event,effects,resources);
    if(recovery.hp>0) reinterpret_cast<int(*)(void*,int,bool)>(base+0x7245B0)(actor,resources.hp+recovery.hp,false);
    if(recovery.mp>0) reinterpret_cast<int(*)(void*,int,bool)>(base+0x724800)(actor,resources.mp+recovery.mp,false);
    if(recovery.hp || recovery.mp) log({{"event","combat_affix_recovery"},{"actor",fname(read<uint64_t>(reinterpret_cast<uintptr_t>(actor)+0x38))},
        {"root_action",event.action},{"hp",recovery.hp},{"mp",recovery.mp},{"turn_end",event.turn_end}});
}
std::string combat_origin_weapon(uintptr_t actor,const Json& tags) {
    if(!manager_object_is_a(actor,"JackFriendGameCharacter")) return {};
    const auto slots=read<Array>(actor+0x5D8);if(slots.count<0 || slots.count>32 || slots.capacity<slots.count) throw std::runtime_error("Invalid combat hand array");
    std::vector<itemization::NativeWeaponSlot> hands;
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    for(int i=0;i<slots.count;++i) {
        const auto at=slots.data+uintptr_t(i)*16;const auto id=read<uint64_t>(at);const auto region=read<uint8_t>(at+8);
        if(region!=1 && region!=2) continue;
        const auto generated=runtime_ids.find(id),last=runtime_ids.end();
        if(generated!=last) {const auto item=item_service->lookup(generated->second);hands.push_back({generated->second,item.at("category"),region});}
        else {const auto vanilla=runtime_bases.find(fname(id));if(vanilla!=runtime_bases.end()) hands.push_back({fname(id),vanilla->second.at("category"),region});}
    }
    const auto origin=itemization::native_hit_origin(hands,tags.value("off_hand",false));return origin.valid?origin.weapon:std::string{};
}
using CombatConsumeFn=void(*)(void*);
using CombatCommitFn=void(*)(void*);
using CombatDelegateFn=void(*)(void*,void*);
using CombatCostFn=int(*)(void*,int,bool);
CombatConsumeFn original_combat_consume=nullptr;
CombatCommitFn original_combat_commit=nullptr;
CombatDelegateFn original_combat_delegate=nullptr;
CombatCostFn original_combat_cost=nullptr;
thread_local bool combat_equipment_event=false;
void combat_consume_hook(void* execution) {
    if(runtime_enabled.load() && combat_events_enabled && execution) try {
        std::lock_guard<std::recursive_mutex> lock(combat_event_mutex);
        const auto exec_id=combat_weak_id(execution);
        if(combat_started_execs.insert(exec_id).second) {
            void* caster=combat_weak_actor(reinterpret_cast<uintptr_t>(execution)+0x500);
            if(caster) {
                const auto actor=reinterpret_cast<uintptr_t>(caster);
                auto& action=combat_actor_actions[actor];
                // Other requests are child/repeat executions of the current
                // turn action. Keep one proc and life-steal budget across them.
                const auto request=read<uint8_t>(reinterpret_cast<uintptr_t>(execution)+0x7A1);
                if(action.root && request!=1) {action.exec=exec_id;original_combat_consume(execution);return;}
                action={++combat_action_sequence,exec_id,false};
                auto momentum=combat_momentum_expiry.find(actor);
                if(momentum!=combat_momentum_expiry.end()) {
                    action.momentum=combat_actor_turns[actor]<=momentum->second;combat_momentum_expiry.erase(momentum);
                }
            }
        }
    } catch(const std::exception& error) {dispatch_error(error.what());}
    original_combat_consume(execution);
}
int combat_cost_hook(void* actor,int cost,bool free_proc) {
    const int native=original_combat_cost(actor,cost,free_proc);
    if(!runtime_enabled.load() || !combat_dispatch_enabled || !actor || native<=0) return native;
    try {return itemization::affix_mp_cost(native,dispatch_equipment(reinterpret_cast<uintptr_t>(actor)),true,native_scalar_balance);}
    catch(const std::exception& error) {dispatch_error(error.what());return native;}
}
void combat_apply_ailments(void* source,void* target,const itemization::AffixEvent& event,const itemization::CombatAffixes& effects) {
    if(!event.landed || !event.weapon_hit || event.metal || combat_resources(target).hp<=0) return;
    // Native hard protection and susceptibility take priority over affix rolls.
    if(dispatch_odd(target,37) || dispatch_odd(target,47)) return;
    std::map<std::string,double> resistance;
    std::set<std::string> immunity;
    for(const auto& entry:std::map<std::string,uint8_t>{{"poison",uint8_t(8)},{"sleep",uint8_t(9)},{"defence_down",uint8_t(29)}}) {
        const double native=dispatch_attribute_hook(target,entry.second)/100;
        resistance[entry.first]=native;if(native<=0) immunity.insert(entry.first);
    }
    const auto statuses=combat_event_executor.ailments(event,effects,resistance,immunity,combat_event_random);
    for(const auto& status:statuses) {
        // Native SetOddEffect return value contains an owned array at +0x10.
        // The same output layout and release are used by native module type 7.
        alignas(8) std::array<uint8_t,0x28> result{};
        void* executor=reinterpret_cast<void*(*)(void*)>(base+0x7E9AF0)(target);
        reinterpret_cast<void(*)(void*,void*,void*,uint64_t,uint8_t,void*,bool)>(base+0x8E6E80)(executor,result.data(),target,combat_native_statuses.at(status),2,source,false);
        const auto owned=read<uintptr_t>(reinterpret_cast<uintptr_t>(result.data())+0x10);
        if(owned) reinterpret_cast<void(*)(void*)>(base+0xCBDFB0)(reinterpret_cast<void*>(owned));
        log({{"event","combat_affix_ailment"},{"status",status},{"root_action",event.action},{"native_accepted",result[0]!=0}});
    }
}
void combat_commit_hook(void* module) {
    if(!runtime_enabled.load() || !combat_events_enabled || combat_equipment_event || !module) {original_combat_commit(module);return;}
    void* source=nullptr;void* target=nullptr;int hp_before=0;uint8_t type=0;Json tags;itemization::AffixEvent event;itemization::CombatAffixes outgoing,defensive;
    bool eligible=false;
    try {
        std::lock_guard<std::recursive_mutex> lock(combat_event_mutex);
        const auto address=reinterpret_cast<uintptr_t>(module);
        type=read<uint8_t>(address);source=combat_weak_actor(address+4);target=combat_weak_actor(address+0xC);
        if(source && target && (type==1 || type==10 || type==11 || type==12)) {
            const auto actor=reinterpret_cast<uintptr_t>(source);
            // Native module +0x48 is the resolved action ID; +0x3A is its
            // critical decision, copied from the common calculator's +0x40.
            const auto action_name=fname(read<uint64_t>(address+0x48));
            tags=itemization::resolve_native_action_tags(native_scalar_actions,native_scalar_damage_rows,action_name,"None");
            if(tags.is_object()) {
                auto current=combat_actor_actions.find(actor);
                if(current==combat_actor_actions.end()) {
                    // Scripted native delivery can omit MP consumption. Its
                    // committed modules share the wearer-turn root until exit.
                    current=combat_actor_actions.emplace(actor,NativeActionEvents{++combat_action_sequence,0,false}).first;
                }
                event.battle=combat_battle_generation;event.action=current->second.root;event.hit=++combat_hit_sequence;
                event.target=combat_weak_id(target);event.actor=std::to_string(combat_weak_id(source));event.turn=combat_actor_turns[actor];
                event.physical=tags.value("physical",false);event.critical=read<uint8_t>(address+0x3A)!=0;
                const auto origin=combat_origin_weapon(actor,tags);
                const auto category=tags.value("action_category",std::string{});
                const auto selector=tags.value("calculation_type",std::string{});
                event.weapon_hit=!origin.empty() && event.physical &&
                    (category=="EJackActionCategory__Kougeki" || category=="EJackActionCategory__Tokugi") &&
                    (selector=="EJackActionDamageCalcType__Kougeki" || selector=="EJackActionDamageCalcType__Standard" || selector=="EJackActionDamageCalcType__Boo");
                outgoing=dispatch_equipment(actor,origin);defensive=dispatch_equipment(reinterpret_cast<uintptr_t>(target));
                const auto monster=dispatch_monsters.find(fname(read<uint64_t>(reinterpret_cast<uintptr_t>(target)+0x38)));
                event.metal=monster!=dispatch_monsters.end() && monster->value("metal",false);
                hp_before=read<int>(reinterpret_cast<uintptr_t>(target)+0x7C);eligible=true;
            }
        }
    } catch(const std::exception& error) {dispatch_error(error.what());}
    original_combat_commit(module); // Exactly one native application.
    if(!eligible) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(combat_event_mutex);
        struct EventGuard {EventGuard(){combat_equipment_event=true;}~EventGuard(){combat_equipment_event=false;}} guard;
        const int after=read<int>(reinterpret_cast<uintptr_t>(target)+0x7C);
        event.actual_hp_damage=std::max(0,hp_before-after);event.landed=type==1 && event.actual_hp_damage>0;
        event.defeated=type==1 && hp_before>0 && after==0;
        combat_apply_recovery(source,event,outgoing);
        combat_apply_ailments(source,target,event,outgoing);
        if(type==10 || type==11 || type==12) {
            auto defender=event;defender.actor=std::to_string(combat_weak_id(target));defender.turn=combat_actor_turns[reinterpret_cast<uintptr_t>(target)];
            defender.target=combat_weak_id(source);defender.blocked=type==11 || type==12;defender.dodged=type==10;
            defender.critical=false;defender.landed=false;defender.defeated=false;
            combat_apply_recovery(target,defender,defensive);
            if(defender.dodged && defensive.get("evasion_momentum")>0) combat_momentum_expiry[reinterpret_cast<uintptr_t>(target)]=defender.turn+1;
        }
    } catch(const std::exception& error) {dispatch_error(error.what());}
}
void combat_delegate_hook(void* delegate,void* parameters) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    original_combat_delegate(delegate,parameters);
    if(!runtime_enabled.load() || !combat_events_enabled) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(combat_event_mutex);
        const auto address=reinterpret_cast<uintptr_t>(delegate);
        if(caller==base+0x4DAC52) { // Native wearer-turn post-action exit.
            combat_event_manager=address-0x590;
            void* actor=read<void*>(reinterpret_cast<uintptr_t>(parameters));if(!actor) return;
            const auto at=reinterpret_cast<uintptr_t>(actor);
            itemization::AffixEvent event;event.actor=std::to_string(combat_weak_id(actor));event.battle=combat_battle_generation;
            event.turn=++combat_actor_turns[at];event.turn_end=true;
            combat_actor_actions.erase(at);
            combat_apply_recovery(actor,event,dispatch_equipment(at));
        } else if(combat_event_manager && (address==combat_event_manager+0x570 || address==combat_event_manager+0x5E0 || address==combat_event_manager+0x620)) {
            combat_reset_events();
        }
    } catch(const std::exception& error) {dispatch_error(error.what());}
}
const std::vector<DispatchBinding>& combat_event_bindings() {
    static const std::vector<DispatchBinding> bindings={
        {0x411010,reinterpret_cast<void*>(&combat_consume_hook),reinterpret_cast<void**>(&original_combat_consume),{0x40,0x53,0x48,0x83,0xEC}},
        {0x43C3F0,reinterpret_cast<void*>(&combat_commit_hook),reinterpret_cast<void**>(&original_combat_commit),{0x40,0x55,0x56,0x57,0x41}},
        {0x3F7100,reinterpret_cast<void*>(&combat_delegate_hook),reinterpret_cast<void**>(&original_combat_delegate),{0x4C,0x8B,0xDC,0x53,0x56}},
        {0x3D9DC0,reinterpret_cast<void*>(&combat_cost_hook),reinterpret_cast<void**>(&original_combat_cost),{0x48,0x89,0x5C,0x24,0x08}},
    };return bindings;
}
void create_combat_event_hooks() {
    for(const auto& binding:combat_event_bindings()) {
        if(binding.rva!=0x3D9DC0 && !combat_events_enabled) continue;
        for(size_t i=0;i<binding.signature.size();++i) if(read<uint8_t>(base+binding.rva+i)!=binding.signature[i]) throw std::runtime_error("Combat application signature mismatch");
        if(MH_CreateHook(reinterpret_cast<void*>(base+binding.rva),binding.hook,binding.original)!=MH_OK) throw std::runtime_error("Combat application hook creation failed");
    }
}
void enable_combat_event_hooks() {
    for(const auto& binding:combat_event_bindings()) if(binding.rva==0x3D9DC0 || combat_events_enabled)
        if(MH_EnableHook(reinterpret_cast<void*>(base+binding.rva))!=MH_OK) throw std::runtime_error("Combat application activation failed");
}
