// Shared native calculation adapter. All state is call-scoped, including AI
// previews. These hooks do not spend resources or deliver equipment events.
bool combat_dispatch_enabled=false;
Json dispatch_skill_families=Json::object(),dispatch_monsters=Json::object();
std::vector<uint64_t> dispatch_bad_stats;
using DispatchElementFn=bool(*)(float*,uint8_t*,float*,float,void*,void*,void*,uint8_t,uint8_t);
DispatchElementFn dispatch_element_resolver=nullptr;
struct DispatchFrame {
    DispatchFrame* previous=nullptr;
    void* source=nullptr;void* target=nullptr;void* action=nullptr;void* context=nullptr;
    uintptr_t row=0;
    Json tags;
    itemization::CombatAffixes attack,defence;
    itemization::CombatHit hit;
    itemization::NativeHitScale scale;
    uint8_t original_element=0;
    double original_resistance=1;
    bool applicable=false,terminal_applied=false,probing=false;
    int rate_variance=0;
    int spell_critical_percent=0;
};
thread_local DispatchFrame* active_dispatch=nullptr;
std::atomic<uint64_t> dispatch_errors{0};
bool dispatch_momentum_active(uintptr_t actor) noexcept;
void dispatch_error(const char* message) noexcept {
    if(dispatch_errors.fetch_add(1)<100) log({{"event","combat_dispatch_error"},{"reason",message}});
}
itemization::CombatAffixes dispatch_equipment(uintptr_t actor,const std::string& origin={}) {
    if(!actor) return {};
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    return itemization::CombatAffixes::collect(generated_combat_equipment(actor),origin);
}
bool dispatch_odd(void* actor,uint8_t category) {
    return actor && reinterpret_cast<bool(*)(void*,uint8_t)>(base+0x8DAE10)(actor,category);
}
void initialize_combat_dispatch(const std::filesystem::path& folder,const Json& settings) {
    combat_dispatch_enabled=settings.value("native_combat_dispatch",false);
    if(!combat_dispatch_enabled) return;
    const std::array<uint8_t,5> spell_gate_signature{0x48,0x8B,0x41,0x18,0x48};
    for(size_t i=0;i<spell_gate_signature.size();++i)
        if(read<uint8_t>(base+0x422010+i)!=spell_gate_signature[i]) throw std::runtime_error("Spell critical gate signature mismatch");
    if(read<float>(base+0x53A4D48)!=5.0f) throw std::runtime_error("Spell critical constant mismatch");
    std::ifstream tags_file(folder/L"action-affix-tags.json"),monsters_file(folder/L"combat-target-tags.json");
    if(!tags_file || !monsters_file) throw std::runtime_error("Missing shared combat catalog");
    const auto tags=Json::parse(tags_file);dispatch_skill_families=tags.at("skill_families");
    const auto targets=Json::parse(monsters_file);dispatch_monsters=targets.at("monsters");
    for(const auto& status:targets.at("negative_stat_effects")) dispatch_bad_stats.push_back(native_name(status.get<std::string>()));
    // The shared adapter replaces the result scalars and first Fire proof;
    // neither old adapter may run a second time on the same hit.
    added_fire_prototype_enabled=false;
    log({{"event","combat_dispatch_loaded"},{"rounding","native, once"},{"side_effects",false},{"gameplay_verified",false}});
}
struct DispatchScope {
    DispatchFrame frame;
    DispatchScope(void* source,void* target,void* action,void* context) {
        frame.previous=active_dispatch;active_dispatch=&frame;
        if(!runtime_enabled.load() || !combat_dispatch_enabled || !source || !target || !action || !context) return;
        try {
            frame.source=source;frame.target=target;frame.action=action;frame.context=context;
            auto row=read<uintptr_t>(reinterpret_cast<uintptr_t>(action)+0x20);if(!row) return;
            const auto override_id=read<uint64_t>(reinterpret_cast<uintptr_t>(context)+0x20);
            if(override_id) {
                const auto resolved=reinterpret_cast<uintptr_t(*)(void*,uint64_t)>(base+0x41D030)(action,override_id);
                if(resolved) row=resolved; // The same resolver the native calculator uses.
            }
            frame.row=row;
            const auto action_name=fname(read<uint64_t>(reinterpret_cast<uintptr_t>(action)));
            frame.tags=itemization::resolve_native_action_tags(native_scalar_actions,native_scalar_damage_rows,action_name,
                fname(read<uint64_t>(reinterpret_cast<uintptr_t>(context)+0x20)));
            if(!frame.tags.is_object()) return;
            frame.hit.physical=read<uint8_t>(row+0x47)!=0;
            frame.hit.spell=itemization::native_offensive_spell(frame.tags,read<uint8_t>(row+8));
            frame.hit.critical=read<uint8_t>(reinterpret_cast<uintptr_t>(context)+0xC)!=0;
            frame.hit.metal=read<uint8_t>(reinterpret_cast<uintptr_t>(context)+0xD)!=0;
            frame.hit.native_limit=read<int32_t>(row+0x38);if(frame.hit.native_limit<=0) frame.hit.native_limit=99999;
            const auto category=frame.tags.value("action_category",std::string{});
            const auto selector=read<uint8_t>(row+9);
            const bool weapon_action=(category=="EJackActionCategory__Kougeki" || category=="EJackActionCategory__Tokugi") &&
                frame.hit.physical && (selector==1 || selector==2 || selector==7);
            std::string origin,type;
            const uintptr_t actor=reinterpret_cast<uintptr_t>(source);
            {std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
                if(manager_object_is_a(actor,"JackFriendGameCharacter")) {
                    const auto slots=read<Array>(actor+0x5D8);std::vector<itemization::NativeWeaponSlot> hands;
                    if(slots.count<0 || slots.count>32 || slots.capacity<slots.count) throw std::runtime_error("Invalid native hand array");
                    for(int i=0;i<slots.count;++i) {
                        const auto address=slots.data+uintptr_t(i)*16;
                        const auto id=read<uint64_t>(address);const auto region=read<uint8_t>(address+8);
                        if(region!=1 && region!=2) continue;
                        const auto generated=runtime_ids.find(id);
                        const auto vanilla=runtime_bases.find(fname(id));
                        Json item;
                        if(generated!=runtime_ids.end()) item=item_service->lookup(generated->second);
                        else if(vanilla!=runtime_bases.end()) item=vanilla->second;
                        if(item.is_null()) continue;
                        const std::string key=generated!=runtime_ids.end()?generated->second:fname(id);
                        hands.push_back({key,item.at("category"),region});
                        if(region==(read<uint8_t>(row+0x14)?2:1) && item.at("category")=="weapon")
                            type=generated!=runtime_ids.end()?item.at("base_snapshot").value("equipment_type",std::string{}):item.value("equipment_type",std::string{});
                    }
                    const auto selected=itemization::native_hit_origin(hands,read<uint8_t>(row+0x14)!=0);
                    if(selected.valid) origin=selected.weapon;
                }
                frame.attack=dispatch_equipment(actor,origin);
                frame.defence=dispatch_equipment(reinterpret_cast<uintptr_t>(target));
            }
            if(!dispatch_momentum_active(actor)) frame.attack.values.erase("evasion_momentum");
            frame.hit.weapon_hit=!frame.hit.spell && weapon_action && !origin.empty();
            const auto families=dispatch_skill_families.find(action_name);
            if(category=="EJackActionCategory__Tokugi" && families!=dispatch_skill_families.end()) {
                std::string family;for(char c:type) family+=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if(std::find(families->begin(),families->end(),Json(family))!=families->end()) frame.hit.skill_family=family;
            }
            // Weapon-panel energy skills may use a nonphysical Standard row.
            // Their source family still identifies the originating weapon.
            if(!frame.hit.spell && !origin.empty() && !frame.hit.skill_family.empty() && (selector==1 || selector==2 || selector==7)) frame.hit.weapon_hit=true;
            const auto monster=dispatch_monsters.find(fname(read<uint64_t>(reinterpret_cast<uintptr_t>(target)+0x38)));
            if(monster!=dispatch_monsters.end()) frame.hit.enemy_family=monster->value("family",std::string{});
            const int hp=read<int32_t>(actor+0x7C);
            const int max_hp=reinterpret_cast<int(*)(void*)>(base+0x716FB0)(source);
            frame.hit.full_hp=max_hp>0 && hp==max_hp;
            frame.hit.low_hp=max_hp>0 && hp>0 && hp<=max_hp*0.3;
            frame.hit.target_poisoned=dispatch_odd(target,5);
            for(const uint8_t effect:{uint8_t(2),uint8_t(3),uint8_t(4),uint8_t(5),uint8_t(6),uint8_t(7),uint8_t(8),uint8_t(9),uint8_t(10)})
                frame.hit.target_debuffed|=dispatch_odd(target,effect);
            if(!frame.hit.target_debuffed) for(const auto status:dispatch_bad_stats)
                if(reinterpret_cast<bool(*)(void*,uint64_t)>(base+0x8DAEB0)(target,status)) {frame.hit.target_debuffed=true;break;}
            frame.applicable=!frame.attack.values.empty() || !frame.defence.values.empty();
        } catch(const std::exception& e) {frame.applicable=false;dispatch_error(e.what());}
    }
    ~DispatchScope() {active_dispatch=frame.previous;}
};
const char* dispatch_element_name(uint8_t element) {
    static const char* elements[]={"physical","fire","ice","wind","lightning","earth","dark","light"};
    return element<8?elements[element]:"physical";
}
void dispatch_capture_element(void* source,void* target,uint8_t element,float factor,uintptr_t caller) noexcept {
    auto* f=active_dispatch;
    if(!f || !f->applicable || f->probing || source!=f->source || target!=f->target || caller!=base+0x3DAF0C) return;
    if(std::isfinite(factor) && factor>=0 && factor<=1000) {f->original_element=element;f->original_resistance=factor/100;}
}
void dispatch_terminal(float* output,bool critical) noexcept {
    auto* f=active_dispatch;
    if(!f || !f->applicable || f->terminal_applied || f->probing || (f->hit.critical && !critical)) return;
    try {
        const auto type=read<uint8_t>(f->row+8);const auto category=f->tags.value("action_category",std::string{});
        const double before=*output;if(!std::isfinite(before)) throw std::runtime_error("Invalid native float amount");
        double after=before;
        if(type==3) {
            const bool item=category=="EJackActionCategory__Item";
            const bool heal=category=="EJackActionCategory__Jumon" || category=="EJackActionCategory__Tokugi" || item;
            if(heal) after=std::max(0.0,before)*(1+itemization::capped(f->attack.get(item?"item_healing":"healing_power"),native_scalar_balance.cap(item?"item_healing":"healing_power"))/100)*
                (1+itemization::capped(f->defence.get("healing_received"),native_scalar_balance.cap("healing_received"))/100);
        } else if(type==1) {
            if(f->hit.metal) {
                if(critical && f->hit.physical) after=before*(1+itemization::capped(f->attack.get("critical_damage"),native_scalar_balance.cap("damage_increase"))/100);
            } else {
                auto hit=f->hit;
                hit.added_damage_coefficient=f->scale.valid?f->scale.coefficient:0;
                double original=std::max(0.0,before);
                hit.components={{dispatch_element_name(f->original_element),original,false}};
                for(uint8_t element=1;element<=7;++element) {
                    if(itemization::added_element_amount(hit,f->attack,dispatch_element_name(element))<=0) continue;
                    float amount=1,factor=100;uint8_t resolved=0;alignas(8) std::array<uint8_t,0x58> row{};row[0x45]=1;
                    f->probing=true;
                    const bool applied=dispatch_element_resolver(&amount,&resolved,&factor,1,f->source,f->target,row.data(),read<uint8_t>(f->row+0x14)?0:1,element);
                    f->probing=false;
                    if(!applied || resolved!=element || !std::isfinite(factor) || factor<0 || factor>1000) throw std::runtime_error("Unresolved native element resistance");
                    hit.target_element_multipliers[dispatch_element_name(element)]=factor/100;
                }
                // Attribute hooks already include equipment resistance. Do not
                // subtract it again when resolving added components.
                auto defence=f->defence;
                for(uint8_t element=1;element<=7;++element) defence.values.erase(std::string("resistance_")+dispatch_element_name(element));
                after=itemization::native_damage_amount(original,hit,f->attack,defence,native_scalar_balance);
            }
        }
        if(!std::isfinite(after) || after<0) throw std::runtime_error("Invalid combined native amount");
        *output=static_cast<float>(after);f->terminal_applied=true;
    } catch(const std::exception& e) {f->probing=false;dispatch_error(e.what());}
}

using DispatchRateFn=float(*)(float*,float,void*,void*,void*,bool);
using DispatchRandomFn=int(*)(int,int);
using DispatchStage4Fn=bool(*)(float*,float,void*,void*);
using DispatchStage5Fn=bool(*)(float*,float,void*,void*,bool);
using DispatchStage7Fn=bool(*)(float*,float,void*,void*,void*,bool,bool);
using DispatchHitFn=bool(*)(float*,bool*,void*,void*,int);
using DispatchCriticalFn=void(*)(float*,float,void*,void*,void*,void*,bool,void*);
using DispatchBooFn=float(*)(float*,void*,void*,void*,void*,int,bool);
using DispatchAttributeFn=float(*)(void*,uint8_t);
using DispatchRoundFn=int(*)(float);
using DispatchCriticalBaseFn=float(*)(void*);
DispatchRateFn original_dispatch_rate=nullptr;
DispatchRandomFn original_dispatch_random=nullptr;
DispatchStage4Fn original_dispatch_attack=nullptr,original_dispatch_attack53=nullptr,original_dispatch_defence52=nullptr,original_dispatch_breath=nullptr;
DispatchStage5Fn original_dispatch_reduction=nullptr,original_dispatch_increase=nullptr;
DispatchStage7Fn original_dispatch_race=nullptr,original_dispatch_conditional=nullptr;
DispatchHitFn original_dispatch_hit=nullptr;
DispatchCriticalFn original_dispatch_critical=nullptr;
DispatchBooFn original_dispatch_boo=nullptr;
DispatchAttributeFn original_dispatch_attribute=nullptr;
DispatchRoundFn original_dispatch_round=nullptr;
DispatchCriticalBaseFn original_dispatch_critical_base=nullptr;
bool dispatch_main_stage(uintptr_t caller) {
    return active_dispatch && active_dispatch->applicable && !active_dispatch->probing && caller>=base+0x3DAE00 && caller<base+0x3DAFF0;
}
int dispatch_random_hook(int low,int high) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const int result=original_dispatch_random(low,high);
    if(active_dispatch && !active_dispatch->probing && caller==base+0x3DC2A6) active_dispatch->rate_variance=result;
    // Reuse the native spell-critical draw, including a zero original component
    // against an immune target. Never invoke RNG to calculate the added part.
    if(active_dispatch && !active_dispatch->probing && active_dispatch->hit.spell &&
        (caller==base+0x3DA2AB || caller==base+0x3DA2DD || caller==base+0x3DA30F))
        active_dispatch->spell_critical_percent=result;
    return result;
}
float dispatch_rate_hook(float* output,float input,void* source,void* target,void* row,bool fixed) {
    auto* f=active_dispatch;const bool track=f && f->applicable && !f->probing;
    if(track) f->rate_variance=0;
    const float result=original_dispatch_rate(output,input,source,target,row,fixed);
    if(track) try {
        // Probe the already chosen coefficient, not a second damage or RNG roll.
        // Its copied row contains the one native percentage-offset draw.
        alignas(8) std::array<uint8_t,0x58> copy{};
        for(size_t i=0;i<copy.size();++i) copy[i]=read<uint8_t>(reinterpret_cast<uintptr_t>(row)+i);
        float rate=read<float>(reinterpret_cast<uintptr_t>(row)+0xC)+f->rate_variance,zero=0;
        std::memcpy(copy.data()+0xC,&rate,4);std::memcpy(copy.data()+0x10,&zero,4);
        float coefficient=1;f->probing=true;
        original_dispatch_rate(&coefficient,1,source,target,copy.data(),true);
        f->probing=false;f->scale.multiply(coefficient);
    } catch(const std::exception& e) {f->probing=false;f->scale.valid=false;dispatch_error(e.what());}
    return result;
}
bool dispatch_hit_hook(float* output,bool* flag,void* source,void* action,int count) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_dispatch_hit(output,flag,source,action,count);
    if(dispatch_main_stage(caller) && result) active_dispatch->scale.multiply(*output/100.0);
    return result;
}
template<class Call> void dispatch_stage_scale(float input,float output,Call unit_probe) noexcept {
    auto* f=active_dispatch;if(!f) return;
    try {
        double scale=1;
        if(std::abs(input)>0.000001f) scale=output/input;
        else {float unit=1;f->probing=true;unit_probe(&unit);f->probing=false;scale=unit;}
        f->scale.multiply(scale);
    } catch(const std::exception& e) {f->probing=false;f->scale.valid=false;dispatch_error(e.what());}
}
#define DISPATCH_STAGE4(NAME,ORIGINAL) \
bool NAME(float* output,float input,void* actor,void* row) { \
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress()); \
    const bool result=ORIGINAL(output,input,actor,row); \
    if(dispatch_main_stage(caller)) dispatch_stage_scale(input,*output,[&](float* unit){ORIGINAL(unit,1,actor,row);}); \
    return result; \
}
DISPATCH_STAGE4(dispatch_attack_hook,original_dispatch_attack)
DISPATCH_STAGE4(dispatch_attack53_hook,original_dispatch_attack53)
DISPATCH_STAGE4(dispatch_defence52_hook,original_dispatch_defence52)
DISPATCH_STAGE4(dispatch_breath_hook,original_dispatch_breath)
#undef DISPATCH_STAGE4
bool dispatch_reduction_hook(float* output,float input,void* target,void* row,bool flag) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_dispatch_reduction(output,input,target,row,flag);
    if(dispatch_main_stage(caller)) dispatch_stage_scale(input,*output,[&](float* unit){original_dispatch_reduction(unit,1,target,row,flag);});
    return result;
}
bool dispatch_increase_hook(float* output,float input,void* source,void* action,bool hand) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_dispatch_increase(output,input,source,action,hand);
    if(dispatch_main_stage(caller)) {
        dispatch_stage_scale(input,*output,[&](float* unit){original_dispatch_increase(unit,1,source,action,hand);});
        dispatch_terminal(output,false);
    }
    return result;
}
bool dispatch_race_hook(float* output,float input,void* source,void* target,void* row,bool additive,bool hand) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_dispatch_race(output,input,source,target,row,additive,hand);
    if(!additive && dispatch_main_stage(caller)) dispatch_stage_scale(input,*output,[&](float* unit){original_dispatch_race(unit,1,source,target,row,false,hand);});
    return result;
}
bool dispatch_conditional_hook(float* output,float input,void* source,void* target,void* action,bool additive,bool hand) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=original_dispatch_conditional(output,input,source,target,action,additive,hand);
    if(!additive && dispatch_main_stage(caller)) dispatch_stage_scale(input,*output,[&](float* unit){original_dispatch_conditional(unit,1,source,target,action,false,hand);});
    return result;
}
void dispatch_critical_hook(float* output,float input,void* source,void* target,void* action,void* row,bool hand,void* context) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto* f=active_dispatch;
    const bool spell=f && f->applicable && !f->probing && f->hit.spell && caller==base+0x3DB05C;
    if(spell) f->spell_critical_percent=0;
    original_dispatch_critical(output,input,source,target,action,row,hand,context);
    if(spell) try {
        // The native spell branch multiplies its existing float amount. Use its
        // actual factor, rather than the weapon component's fixed 2x policy.
        double zero_factor=1;
        // The final native critical enum branch uses a constant factor. Other
        // multiplicative branches supply their already drawn percentage above.
        const auto info=read<uintptr_t>(reinterpret_cast<uintptr_t>(action)+0x18);
        if(input==0 && !f->spell_critical_percent && info && read<uint8_t>(info+0xCC)==6 &&
            reinterpret_cast<bool(*)(void*)>(base+0x422010)(action)) zero_factor=read<float>(base+0x53A4D48);
        f->hit.elemental_critical_multiplier=itemization::native_spell_critical_factor(input,*output,f->spell_critical_percent,zero_factor);
    } catch(const std::exception& e) {f->scale.valid=false;dispatch_error(e.what());}
    if(active_dispatch && caller==base+0x3DB05C) dispatch_terminal(output,true);
}
float dispatch_critical_base_hook(void* parameters) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const float result=original_dispatch_critical_base(parameters);
    auto* f=active_dispatch;
    // Do this before the native max(1.2 * ordinary, critical) selection. The
    // ordinary branch already includes elemental resistance; applying it to
    // the selected result would resist that branch twice.
    if(f && f->applicable && !f->probing && caller==base+0x3DA283 && f->hit.critical && f->hit.physical && !f->hit.metal && f->original_element>0 && f->original_element<8)
        return static_cast<float>(result*f->original_resistance);
    return result;
}
float dispatch_boo_hook(float* output,void* source,void* target,void* action,void* row,int ordinal,bool fixed) {
    const float result=original_dispatch_boo(output,source,target,action,row,ordinal,fixed);
    auto* f=active_dispatch;if(f && f->applicable && !f->probing) try {
        const bool off=read<uint8_t>(reinterpret_cast<uintptr_t>(row)+0x14)!=0;
        const uint64_t status=read<uint64_t>(base+0x5C05AB8);
        const bool full=reinterpret_cast<uint8_t(*)(void*)>(base+0x41D020)(action)==1 &&
            reinterpret_cast<bool(*)(void*,uint64_t)>(base+0x71BD80)(source,status) &&
            reinterpret_cast<bool(*)(void*,uint64_t,int)>(base+0x8DA230)(source,status,off?2:1);
        if(ordinal<0) throw std::runtime_error("Invalid native target ordinal");
        const double falloff=full?1:read<float>(base+0x36AB368+uintptr_t(std::min(ordinal,4))*4);
        f->scale.multiply(falloff);
    } catch(const std::exception& e) {f->scale.valid=false;dispatch_error(e.what());}
    return result;
}
float dispatch_attribute_hook(void* actor,uint8_t attribute) {
    const float native=original_dispatch_attribute(actor,attribute);
    if(!runtime_enabled.load() || !combat_dispatch_enabled || !actor || !((attribute>=1 && attribute<=7) || attribute==8 || attribute==9 || attribute==10 || attribute==11 || attribute==12 || attribute==21)) return native;
    try {
        const auto effects=dispatch_equipment(reinterpret_cast<uintptr_t>(actor));
        std::string stat;double cap=80;
        if(attribute<=7) stat=std::string("resistance_")+dispatch_element_name(attribute);
        else {cap=90;static const std::map<uint8_t,std::string> statuses={{uint8_t(8),"poison"},{uint8_t(9),"sleep"},{uint8_t(10),"confusion"},{uint8_t(11),"paralysis"},{uint8_t(12),"instant_death"},{uint8_t(21),"silence"}};
            stat="status_resistance_"+statuses.at(attribute);}
        return static_cast<float>(itemization::native_resistance_projection(native,effects.get(stat),cap));
    } catch(const std::exception& e) {dispatch_error(e.what());return native;}
}
int dispatch_round_hook(float amount) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // Healing rows may bypass outgoing mitigation stages. Their common final
    // conversion still receives a float; retain the engine's conversion once.
    if(active_dispatch && active_dispatch->applicable && active_dispatch->row && caller==base+0x3DB0A3 && read<uint8_t>(active_dispatch->row+8)==3) {
        dispatch_terminal(&amount,active_dispatch->hit.critical);
        amount=std::min(amount,float(active_dispatch->hit.native_limit));
    }
    return original_dispatch_round(amount);
}
struct DispatchBinding {uintptr_t rva;void* hook;void** original;std::array<uint8_t,5> signature;};
const std::vector<DispatchBinding>& dispatch_bindings() {
    static const std::vector<DispatchBinding> bindings={
        {0x3DC240,reinterpret_cast<void*>(&dispatch_rate_hook),reinterpret_cast<void**>(&original_dispatch_rate),{0x48,0x89,0x5C,0x24,0x08}},
        {0x7F5800,reinterpret_cast<void*>(&dispatch_random_hook),reinterpret_cast<void**>(&original_dispatch_random),{0x2B,0xD1,0x44,0x8B,0xC9}},
        {0x3DD590,reinterpret_cast<void*>(&dispatch_hit_hook),reinterpret_cast<void**>(&original_dispatch_hit),{0x48,0x8B,0xC4,0x48,0x89}},
        {0x3DC6F0,reinterpret_cast<void*>(&dispatch_attack_hook),reinterpret_cast<void**>(&original_dispatch_attack),{0x48,0x8B,0xC4,0x53,0x48}},
        {0x3DBC90,reinterpret_cast<void*>(&dispatch_attack53_hook),reinterpret_cast<void**>(&original_dispatch_attack53),{0x48,0x8B,0xC4,0x53,0x48}},
        {0x3DBC10,reinterpret_cast<void*>(&dispatch_defence52_hook),reinterpret_cast<void**>(&original_dispatch_defence52),{0x48,0x8B,0xC4,0x53,0x48}},
        {0x3D9590,reinterpret_cast<void*>(&dispatch_breath_hook),reinterpret_cast<void**>(&original_dispatch_breath),{0x48,0x8B,0xC4,0x53,0x48}},
        {0x3DB610,reinterpret_cast<void*>(&dispatch_reduction_hook),reinterpret_cast<void**>(&original_dispatch_reduction),{0x48,0x8B,0xC4,0x53,0x48}},
        {0x3E0730,reinterpret_cast<void*>(&dispatch_increase_hook),reinterpret_cast<void**>(&original_dispatch_increase),{0x4C,0x8B,0xDC,0x53,0x48}},
        {0x3DD460,reinterpret_cast<void*>(&dispatch_race_hook),reinterpret_cast<void**>(&original_dispatch_race),{0x48,0x89,0x5C,0x24,0x08}},
        {0x3DE660,reinterpret_cast<void*>(&dispatch_conditional_hook),reinterpret_cast<void**>(&original_dispatch_conditional),{0x4C,0x89,0x4C,0x24,0x20}},
        {0x3DA140,reinterpret_cast<void*>(&dispatch_critical_hook),reinterpret_cast<void**>(&original_dispatch_critical),{0x48,0x89,0x5C,0x24,0x08}},
        {0x3D94C0,reinterpret_cast<void*>(&dispatch_boo_hook),reinterpret_cast<void**>(&original_dispatch_boo),{0x48,0x89,0x5C,0x24,0x08}},
        {0x711600,reinterpret_cast<void*>(&dispatch_attribute_hook),reinterpret_cast<void**>(&original_dispatch_attribute),{0x48,0x81,0xEC,0xE8,0x00}},
        {0x3F2730,reinterpret_cast<void*>(&dispatch_round_hook),reinterpret_cast<void**>(&original_dispatch_round),{0x0F,0x57,0xC9,0x0F,0x2F}},
        {0x3D8620,reinterpret_cast<void*>(&dispatch_critical_base_hook),reinterpret_cast<void**>(&original_dispatch_critical_base),{0x40,0x53,0x56,0x57,0x48}},
    };return bindings;
}
void create_dispatch_hooks() {
    for(const auto& binding:dispatch_bindings()) {
        if(binding.signature[0]==0) throw std::runtime_error("Unverified shared combat binding");
        for(size_t i=0;i<binding.signature.size();++i) if(read<uint8_t>(base+binding.rva+i)!=binding.signature[i]) throw std::runtime_error("Shared combat signature mismatch");
    }
    for(const auto& binding:dispatch_bindings()) if(MH_CreateHook(reinterpret_cast<void*>(base+binding.rva),binding.hook,binding.original)!=MH_OK)
        throw std::runtime_error("Shared combat hook creation failed");
}
void enable_dispatch_hooks() {
    for(const auto& binding:dispatch_bindings()) if(MH_EnableHook(reinterpret_cast<void*>(base+binding.rva))!=MH_OK) throw std::runtime_error("Shared combat activation failed");
}
