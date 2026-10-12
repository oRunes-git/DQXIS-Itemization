// Keep the Blueprint's pending Yes/No wait across intermediate menu pages.
double forge_menu_clock_ms() {
    static const double frequency=[] {LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return static_cast<double>(value.QuadPart);}();
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);return now.QuadPart*1000.0/frequency;
}
struct ForgeMenuTiming {
    const char* step;bool active=false;
    double started=forge_menu_clock_ms();
    uint64_t scans=manager_full_scans.load(),hits=manager_cache_hits.load();
    explicit ForgeMenuTiming(const char* value):step(value) {}
    ~ForgeMenuTiming() {try {if(active) log({{"event","forge_menu_timing"},{"step",step},
        {"duration_ms",forge_menu_clock_ms()-started},{"manager_full_scans",manager_full_scans.load()-scans},
        {"manager_cache_hits",manager_cache_hits.load()-hits}});} catch(...) {}}
};
using ForgePromptInitFn=void(*)(void*,const uint64_t*,Array*,bool);
using ForgePromptFixedFn=void(*)(void*,const Array*,Array*,bool);
using ForgePromptTextFn=Array*(*)(void*,Array*,uint64_t);
using ForgePromptControlFn=void(*)(void*,uint64_t,void*);
using ForgePromptWidgetFn=void(*)(void*,uint64_t);
using ForgePromptCloseFn=void(*)(void*);
void craft_screen_open(bool salvage,uintptr_t caller=0,uintptr_t forge_controller=0);
bool craft_screen_is_open();
ForgePromptInitFn original_forge_prompt_init=nullptr;
ForgePromptInitFn original_forge_prompt_append=nullptr;
ForgePromptFixedFn original_forge_prompt_fixed_init=nullptr,original_forge_prompt_fixed_append=nullptr;
ForgePromptTextFn original_forge_prompt_text=nullptr;
ForgePromptControlFn original_forge_prompt_control=nullptr;
ForgePromptWidgetFn original_forge_prompt_widget=nullptr;
ForgePromptCloseFn forge_prompt_close=nullptr;
struct ForgePrompt {
    uintptr_t accessor=0;
    uint64_t source=0,text_id=0;
    Json options;
    std::unique_ptr<itemization::ForgeMenu> menu;
    std::unique_ptr<itemization::VeylMenu> veyl_menu;
    std::unique_ptr<itemization::CampCraftMenu> camp_menu;
    uint64_t recipe=0;
    bool recipe_applied=false;
    bool blocked=false;
    bool home=false,home_recipes=false,home_recent=false,quality_confirm=false,fixed_home=false;
    std::string text;
    bool finished=false,rendered=false;
};
std::map<uintptr_t,std::shared_ptr<ForgePrompt>> forge_prompts;
thread_local ForgePrompt* active_forge_prompt=nullptr;
thread_local const std::string* active_forge_failure_text=nullptr;
thread_local uint64_t active_forge_result_text_id=0;
thread_local uintptr_t forge_completion_controller=0;
thread_local int forge_completion_answer=1;
thread_local bool forge_completion_return_focus=true;
std::string camp_home_text(const ForgePrompt& prompt) {
    if(prompt.home_recipes) return std::string("Forge equipment\r\nPlay the forge minigame to determine quality.<select>\r\n")+
        (prompt.home_recent?"Recently learned recipes\r\n":"")+"Weapons\r\nArmour\r\nAccessories\r\nBack<select_end><end>";
    return "Fun-Size Forge\r\nChoose a crafting method.<select>\r\nForge equipment\r\nReforge Quality\r\nCrafting\r\nSalvage equipment\r\nLeave<select_end><end>";
}
// Scope the native whole-page renderer to custom crafting choices. Init copies
// these defaults into +0x2E0/+0x2E4; restoring them afterwards leaves the parsed
// custom page instant while subsequent story/result messages keep their settings.
class ForgePromptInstantPage {
    uintptr_t address;uint8_t mode;float fade;
public:
    explicit ForgePromptInstantPage(void* controller):address(reinterpret_cast<uintptr_t>(controller)) {
        static std::set<uintptr_t> verified;
        const auto cls=read<uintptr_t>(address+0x10);
        if(!verified.count(cls)) {
            const std::map<std::string,int> expected{{"DefaultOutputMode",0x1B2},{"DefaultFadeInTime",0x1B4}};
            std::set<std::string> matched;
            int depth=0;
            for(auto parent=cls;parent && depth++<32;parent=read<uintptr_t>(parent+0x40)) {
            auto field=read<uintptr_t>(parent+0x48);
            for(int budget=2048;field && budget>0;--budget,field=read<uintptr_t>(field+0x38)) {
                const auto label=fname(read<uint64_t>(field+0x18));auto found=expected.find(label);
                if(found!=expected.end()) {
                    if(read<int32_t>(field+0x54)!=found->second) throw std::runtime_error("Custom forge text property mismatch");
                    matched.insert(label);
                }
            }
            }
            if(matched.size()!=expected.size()) throw std::runtime_error("Custom forge text properties missing");
            const std::array<uint8_t,7> copy_mode{0x0F,0xB6,0x83,0xB2,0x01,0x00,0x00};
            const std::array<uint8_t,6> copy_fade{0x8B,0x83,0xB4,0x01,0x00,0x00};
            for(size_t i=0;i<copy_mode.size();++i) if(read<uint8_t>(base+0xA0B2FB+i)!=copy_mode[i]) throw std::runtime_error("Custom forge text mode binding mismatch");
            for(size_t i=0;i<copy_fade.size();++i) if(read<uint8_t>(base+0xA0B308+i)!=copy_fade[i]) throw std::runtime_error("Custom forge text fade binding mismatch");
            verified.insert(cls);
        }
        mode=read<uint8_t>(address+0x1B2);fade=read<float>(address+0x1B4);
        *reinterpret_cast<uint8_t*>(address+0x1B2)=2; // Native FadeInPage enum, zero duration.
        *reinterpret_cast<float*>(address+0x1B4)=0;
    }
    ~ForgePromptInstantPage() {
        *reinterpret_cast<uint8_t*>(address+0x1B2)=mode;*reinterpret_cast<float*>(address+0x1B4)=fade;
    }
};
void forge_prompt_render(void* controller,const uint64_t* id,Array* arguments,bool focus,ForgePromptInitFn native,bool custom) {
    std::unique_ptr<ForgePromptInstantPage> instant;
    if(custom) try {instant=std::make_unique<ForgePromptInstantPage>(controller);}
        catch(const std::exception& error) {log({{"event","targeted_forge_instant_text_unavailable"},{"reason",error.what()}});}
    native(controller,id,arguments,focus);
}
void forge_prompt_close_hook(void* controller) {
    try {
    if(forge_completion_controller==reinterpret_cast<uintptr_t>(controller)) {
        // Both control and widget-back handlers write their own raw indices.
        // Map that index BEFORE CloseSelectWindow broadcasts OnOpenClose, so
        // both the immediate list-focus callback and later latent wait see No/Yes.
        const int raw=read<int32_t>(reinterpret_cast<uintptr_t>(controller)+0x344);
        *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(controller)+0x344)=forge_completion_answer;
        log({{"event","targeted_forge_close_answer"},{"raw_index",raw},{"native_answer",forge_completion_answer}});
        if(forge_completion_answer==1 && forge_completion_return_focus) {
            // A custom <select> page does not inherit <yesno>'s return-focus
            // contract. Audited native state 0x600 (A19DA0) resolves the existing
            // PrevFocusWidget weak pointer and returns input when page+0xCF=1,
            // before delivering message completion. Retain the engine's own
            // lifetime/serial checks and focus method instead of forcing a pointer.
            const auto pages=read<Array>(reinterpret_cast<uintptr_t>(controller)+0x290);
            const int page=read<int32_t>(reinterpret_cast<uintptr_t>(controller)+0x2F8);
            if(page<0 || page>=pages.count || pages.count>64 || pages.capacity<pages.count)
                throw std::runtime_error("Invalid cancellation focus-return page");
            *reinterpret_cast<uint8_t*>(pages.data+uintptr_t(page)*0x100+0xCF)=1;
            log({{"event","targeted_forge_return_focus_requested"},{"previous_focus_index",read<int32_t>(reinterpret_cast<uintptr_t>(controller)+0x418)}});
        }
    }
    } catch(const std::exception& error) {log({{"event","targeted_forge_close_failed"},{"reason",error.what()}});}
    forge_prompt_close(controller);
}
void forge_prompt_page_log(const ForgePrompt& prompt) {
    Json rows=Json::array();
    if(prompt.veyl_menu) for(const auto& row:prompt.veyl_menu->rows()) rows.push_back({{"label",row.label},{"available",row.available}});
    else if(prompt.camp_menu) for(const auto& row:prompt.camp_menu->rows()) rows.push_back({{"label",row.label},{"available",row.available}});
    else if(prompt.menu) for(const auto& row:prompt.menu->rows()) rows.push_back({{"label",row.label},{"available",row.available}});
    log({{"event","targeted_forge_page"},{"source",fname(prompt.source)},
        {"stage",prompt.home?"campsite":prompt.camp_menu?prompt.camp_menu->stage_name():prompt.veyl_menu?prompt.veyl_menu->stage_name():prompt.menu?prompt.menu->stage_name():"confirmation"},{"rows",rows},{"text",prompt.text}});
}
Array* forge_prompt_text_hook(void* manager,Array* output,uint64_t id) {
    try {
        if(camp_policy && fname(id)=="Menu_Kazi_Menu_000") return runtime_text(output,"Perfectionist's Pearls");
        if(camp_policy && fname(id)=="Menu_Kazi_Command_003") return runtime_text(output,"Reforge Quality");
        if(camp_policy && fname(id)=="Menu_Kazi_Menu_004") return runtime_text(output,"Pearls Needed");
        if(camp_policy && fname(id)=="Menu_Kazi_Message_010") return runtime_text(output,std::string(camp_mode_name(camp_mode))+": Choose equipment.");
        if(active_forge_failure_text && id==active_forge_result_text_id)
            return runtime_text(output,*active_forge_failure_text);
        if (active_forge_prompt && id==active_forge_prompt->text_id) {
            auto result=runtime_text(output,active_forge_prompt->text);
            active_forge_prompt->rendered=true;return result;
        }
    } catch (const std::exception& error) {log({{"event","targeted_forge_text_failed"},{"reason",error.what()}});}
    return original_forge_prompt_text(manager,output,id);
}
void forge_prompt_apply(void* controller,const uint64_t* id,Array* arguments,bool focus,ForgePromptInitFn native,const char* entry_point) {
    ForgeMenuTiming timing(entry_point);
    std::shared_ptr<ForgePrompt> prompt;
    std::string failure_text;
    try {
        if(runtime_enabled.load() && id) {const auto name=fname(*id);timing.active=name=="Menu_Kazi_Message_007" || name=="Menu_Kazi_Message_042" || name=="Menu_Kazi_Message_046" || name=="Menu_Kazi_Message_047";}
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        forge_prompts.erase(reinterpret_cast<uintptr_t>(controller));
        if(camp_policy && runtime_enabled.load() && id && (fname(*id)=="Menu_Kazi_Message_000" || fname(*id)=="Menu_Kazi_Message_000a")) {
            prompt=std::make_shared<ForgePrompt>();prompt->text_id=*id;prompt->home=true;prompt->home_recent=fname(*id)=="Menu_Kazi_Message_000a";
            camp_mode=itemization::CampMode::Quality;prompt->text=camp_home_text(*prompt);forge_prompts[reinterpret_cast<uintptr_t>(controller)]=prompt;
        }
        if(veyl_policy && !camp_policy && runtime_enabled.load() && id && fname(*id)=="Menu_Kazi_Message_007" && recent_forge_accessor) {
            const auto group=read<Array>(recent_forge_accessor+0x228);
            prompt=std::make_shared<ForgePrompt>();prompt->accessor=recent_forge_accessor;prompt->text_id=*id;
            prompt->recipe=read<uint64_t>(recent_forge_accessor+0x220);
            if(group.count<1 || group.count>16 || group.capacity<group.count) throw std::runtime_error("Instant recipe result group missing");
            const auto base_id=fname(read<uint64_t>(group.data));const auto b=runtime_bases.find(base_id);
            if(b==runtime_bases.end() || !b->second.value("automatic_acquisition",true)) {
                prompt->recipe_applied=true;
                prompt->text="This recipe uses a protected base and is not available for currency crafting.<select>Return to recipes</select><end>";
            } else prompt->text="Create "+b->second.at("display_name").get<std::string>()+"?\r\nConsumes the listed recipe materials.\r\nCreates Normal equipment instantly.<select>Create equipment\r\nCancel</select><end>";
            forge_prompts[reinterpret_cast<uintptr_t>(controller)]=prompt;
        }
        if(camp_policy && runtime_enabled.load() && id && recent_forge_accessor &&
            (fname(*id)=="Menu_Kazi_Message_046" || fname(*id)=="Menu_Kazi_Message_047" || fname(*id)=="Menu_Kazi_Message_015" || fname(*id)=="Menu_Kazi_Message_016")) {
            const auto found=forge_sessions.find(recent_forge_accessor);
            if(found!=forge_sessions.end() && found->second.prepared && found->second.performance_measured) {
                const auto item=item_service->lookup(runtime_ids.at(found->second.result_id));
                failure_text=itemization::instance_name(item)+"\r\nQuality: "+itemization::display_number(itemization::quality_percent(item))+
                    "%  |  Score: "+itemization::forge_feedback_score(found->second.performance.score)+" / 100\r\nImplicits and modifiers receive the quality bonus.<end>";
            }
        }
        if(!camp_policy && forge_modifiers_enabled && runtime_enabled.load() && id && arguments && recent_forge_accessor &&
            (fname(*id)=="Menu_Kazi_Message_046" || fname(*id)=="Menu_Kazi_Message_047")) {
            auto session=forge_sessions.find(recent_forge_accessor);
            bool result_ready=session!=forge_sessions.end() && session->second.committed;
            if(session!=forge_sessions.end() && session->second.retry && session->second.modded &&
                session->second.prepared && session->second.performance_measured && !session->second.committed &&
                session->second.outcome>0 && fname(*id)=="Menu_Kazi_Message_046") {
                // Success message 046 opens before ReplaceRetryKaziItem. Display
                // the validated prepared result without advancing the commit or
                // changing focus. Failure 047 retains its committed-result gate.
                const auto& attempt=session->second;const auto manager=find_manager();
                result_ready=read<uint64_t>(recent_forge_accessor+0x430)==attempt.old_id &&
                    read<int32_t>(recent_forge_accessor+0x41C)==attempt.outcome && owned_count(manager,attempt.old_id)==1 &&
                    (attempt.result_id==attempt.old_id || owned_count(manager,attempt.result_id)==0);
            }
            if(session!=forge_sessions.end() && session->second.retry && session->second.modded &&
                session->second.prepared && result_ready &&
                ((session->second.performance_measured && forge_policy->uses_performance() &&
                    ((session->second.outcome==0)==(fname(*id)=="Menu_Kazi_Message_047"))) ||
                (session->second.outcome==0 && (session->second.choice.operation=="replace_affix" || session->second.choice.operation=="reroll_value") && fname(*id)=="Menu_Kazi_Message_047"))) {
                const auto& attempt=session->second;const auto args=read<Array>(reinterpret_cast<uintptr_t>(arguments));
                bool matches=false;
                if(args.count>=0 && args.count<=128 && args.capacity>=args.count) for(int i=0;i<args.count;++i) {
                    const auto argument=read<MessageArgumentView>(args.data+uintptr_t(i)*sizeof(MessageArgumentView));
                    if(argument.table_type==2 && (argument.id==attempt.old_id || argument.id==attempt.result_id)) matches=true;
                }
                if(matches) {
                    const auto item=item_service->lookup(runtime_ids.at(attempt.result_id));
                    const auto previous_item=item_service->lookup(runtime_ids.at(attempt.old_id));
                    if(attempt.performance_measured && forge_policy->uses_performance()) {
                        const auto decision=forge_policy->performance_decision(*item_service,previous_item,attempt.choice,attempt.performance.score);
                        failure_text=itemization::forge_performance_result_text(*item_service,previous_item,item,attempt.choice,decision);
                    } else for(size_t i=0;i<previous_item.at("affixes").size();++i) if(previous_item.at("affixes")[i].at("id")==attempt.choice.affix_id) {
                        const auto& affix=item.at("affixes").at(i);
                        const auto tier=item_service->affix_tier(affix);
                        const bool value_reroll=attempt.choice.operation=="reroll_value";
                        failure_text=std::string(value_reroll?"Reworking failed. The value was reduced to its tier minimum.\r\n":"Reworking failed. The modifier was randomized.\r\n")+itemization::instance_name(item)+"\r\n"+
                            itemization::display_stat_label(affix.at("label"))+" +"+number_text(affix.at("value"))+
                            (affix.at("unit")=="percentage_points"?"%":"")+" (T"+
                            std::to_string(tier.at("rank").get<int>())+": "+number_text(tier.at("minimum"))+"-"+number_text(tier.at("maximum"))+")\r\n"+
                            (value_reroll?"The tier is unchanged.<end>":"Tier 1 is the minimum.<end>");
                    }
                    log({{"event",attempt.performance_measured?"targeted_forge_performance_message":"targeted_forge_failure_message"},{"engine_id",item.at("engine_id")},{"text",failure_text}});
                }
            }
        }
        if (forge_modifiers_enabled && runtime_enabled.load() && id && arguments && recent_forge_accessor &&
            fname(read<uint64_t>(reinterpret_cast<uintptr_t>(id)))=="Menu_Kazi_Message_042") {
            const auto args=read<Array>(reinterpret_cast<uintptr_t>(arguments));
            if (args.count<0 || args.count>128 || args.capacity<args.count) throw std::runtime_error("Invalid crafting prompt arguments");
            uint64_t source=0;
            for (int i=0;i<args.count;++i) {
                const auto argument=read<MessageArgumentView>(args.data+uintptr_t(i)*sizeof(MessageArgumentView));
                if (argument.table_type==2 && forge_custom_item(argument.id)) {
                    if (source && source!=argument.id) throw std::runtime_error("Ambiguous crafting prompt item");
                    source=argument.id;
                }
            }
            if (source) {
                prompt=std::make_shared<ForgePrompt>();prompt->source=source;prompt->accessor=recent_forge_accessor;prompt->text_id=*id;
                forge_plans.erase(prompt->accessor);
                const auto item=item_service->lookup(runtime_ids.at(source));
                if (owned_count(find_manager(),source)!=1) throw std::runtime_error("Crafting prompt source is not uniquely owned");
                if(camp_policy) {
                    camp_mode=itemization::CampMode::Quality;prompt->quality_confirm=true;
                    const auto cost=forge_base_pearl_cost(reinterpret_cast<void*>(prompt->accessor),source);
                    prompt->text=itemization::instance_name(item)+"\r\nReforge Quality: score 0-100 gives 0-25%.\r\nReplaces quality; keeps modifiers and item base. Costs "+std::to_string(cost)+" pearls.<select>\r\nReforge\r\nCancel<select_end><end>";
                } else if(camp_policy && camp_mode!=itemization::CampMode::Veyl) {
                    const auto manager=find_manager();
                    if((camp_mode==itemization::CampMode::Salvage || camp_mode==itemization::CampMode::MassSalvage) &&
                        camp_mode==itemization::CampMode::Salvage && !salvage_source_valid(manager,source)) throw std::runtime_error("Equipped or protected gear cannot be salvaged");
                    prompt->camp_menu=std::make_unique<itemization::CampCraftMenu>(*item_service,*camp_policy,camp_mode,item,shard_wallet(manager),
                        camp_mode==itemization::CampMode::MassSalvage?salvage_inventory(manager):std::vector<Json>{});
                    prompt->text=prompt->camp_menu->text();
                } else if(veyl_policy) {
                    prompt->veyl_menu=std::make_unique<itemization::VeylMenu>(*item_service,*veyl_policy,item,veyl_wallet(find_manager()));
                    prompt->text=prompt->veyl_menu->text();
                } else {
                const auto accessor=reinterpret_cast<void*>(prompt->accessor);
                const auto native_cost=original_forge_cost(accessor,source);
                const auto pearls=original_forge_pearls(accessor,0);
                prompt->options=Json::array();
                for (const auto& option:forge_navigation_cache().options(runtime_ids.at(source),native_cost))
                    if (option.at("cost").get<int>()<=pearls) prompt->options.push_back(option);
                prompt->menu=std::make_unique<itemization::ForgeMenu>(*item_service,item,prompt->options,6);
                prompt->text=prompt->menu->text();
                }
                forge_prompts[reinterpret_cast<uintptr_t>(controller)]=prompt;
                log({{"event","targeted_forge_prompt"},{"entry_point",entry_point},{"engine_id",item.at("engine_id")},{"options",prompt->options},{"currency_mode",bool(prompt->veyl_menu)}});
                forge_prompt_page_log(*prompt);
            }
        }
    } catch (const std::exception& error) {
        log({{"event","targeted_forge_prompt_failed"},{"reason",error.what()}});
        if(veyl_policy && id && (fname(*id)=="Menu_Kazi_Message_042" || fname(*id)=="Menu_Kazi_Message_007")) {
            prompt=std::make_shared<ForgePrompt>();prompt->text_id=*id;prompt->accessor=recent_forge_accessor;prompt->blocked=true;
            prompt->text="Crafting is unavailable. Nothing was spent or changed.<select>Return</select><end>";
            forge_prompts[reinterpret_cast<uintptr_t>(controller)]=prompt;
        } else prompt.reset();
    }
    auto previous=active_forge_prompt;active_forge_prompt=prompt.get();
    auto previous_failure=active_forge_failure_text;active_forge_failure_text=failure_text.empty()?nullptr:&failure_text;
    const auto previous_result_text_id=active_forge_result_text_id;active_forge_result_text_id=id?*id:0;
    // SetTextAtTextID uses AppendTextAtTextID once a window is already open.
    // A new generated-item menu replaces the completed pages; it must not resume
    // an old selection or fall through to an unplanned native rework confirmation.
    forge_prompt_render(controller,id,arguments,focus,prompt?original_forge_prompt_init:native,bool(prompt));
    active_forge_failure_text=previous_failure;active_forge_result_text_id=previous_result_text_id;
    active_forge_prompt=previous;
    if (prompt && !prompt->rendered) log({{"event","targeted_forge_prompt_not_rendered"}});
}
void forge_prompt_init_hook(void* controller,const uint64_t* id,Array* arguments,bool focus) {
    forge_prompt_apply(controller,id,arguments,focus,original_forge_prompt_init,"init");
}
void forge_prompt_append_hook(void* controller,const uint64_t* id,Array* arguments,bool focus) {
    forge_prompt_apply(controller,id,arguments,focus,original_forge_prompt_append,"append");
}
void forge_fixed_home_render(void* controller,const std::shared_ptr<ForgePrompt>& prompt,Array* arguments,bool focus) {
    const auto script=wide_utf8(prompt->text);
    const Array text{reinterpret_cast<uintptr_t>(script.c_str()),static_cast<int>(script.size()+1),static_cast<int>(script.size()+1)};
    std::unique_ptr<ForgePromptInstantPage> instant;
    try {instant=std::make_unique<ForgePromptInstantPage>(controller);}
    catch(const std::exception& error) {log({{"event","targeted_forge_instant_text_unavailable"},{"reason",error.what()}});}
    original_forge_prompt_fixed_init(controller,&text,arguments,focus);
    prompt->rendered=true;
}
void forge_prompt_fixed_apply(void* controller,const Array* text,Array* arguments,bool focus,ForgePromptFixedFn native) {
    std::shared_ptr<ForgePrompt> prompt;
    try {
        if(camp_policy && runtime_enabled.load() && text && text->data && text->count>0 && text->count<=4097 && text->capacity>=text->count) {
            const auto script=std::wstring_view(reinterpret_cast<const wchar_t*>(text->data),text->count-1);
            const auto variant=itemization::forge_home_variant(script);
            if(variant!=itemization::ForgeHomeVariant::None) {
                std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
                prompt=std::make_shared<ForgePrompt>();prompt->home=true;prompt->fixed_home=true;
                prompt->home_recent=variant==itemization::ForgeHomeVariant::RecentRecipes;
                camp_mode=itemization::CampMode::Quality;prompt->text=camp_home_text(*prompt);
                forge_prompts[reinterpret_cast<uintptr_t>(controller)]=prompt;
            }
        }
    } catch(const std::exception& e) {log({{"event","campsite_forge_home_refused"},{"reason",e.what()}});}
    if(!prompt) {if(!active_forge_prompt)forge_prompts.erase(reinterpret_cast<uintptr_t>(controller));native(controller,text,arguments,focus);return;}
    forge_fixed_home_render(controller,prompt,arguments,focus);
    forge_prompt_page_log(*prompt);
    log({{"event","campsite_forge_home_bound"},{"message_path","fixed_text"},{"recent_recipes",prompt->home_recent}});
}
void forge_prompt_fixed_init_hook(void* controller,const Array* text,Array* arguments,bool focus) {
    forge_prompt_fixed_apply(controller,text,arguments,focus,original_forge_prompt_fixed_init);
}
void forge_prompt_fixed_append_hook(void* controller,const Array* text,Array* arguments,bool focus) {
    forge_prompt_fixed_apply(controller,text,arguments,focus,original_forge_prompt_fixed_append);
}
void forge_prompt_next_page(void* controller,const std::shared_ptr<ForgePrompt>& prompt) {
    // Original CloseSelectWindow and InitTextAtTextID retain the latent result.
    // OnSelectWindowControl would advance to message completion, so skip it here.
    forge_prompt_close(controller);
    if(prompt->home) prompt->text=camp_home_text(*prompt);else if(prompt->camp_menu) prompt->text=prompt->camp_menu->text();
    else if(prompt->veyl_menu) prompt->text=prompt->veyl_menu->text();else if(prompt->menu) prompt->text=prompt->menu->text();
    prompt->rendered=false;
    Array no_arguments{};auto previous=active_forge_prompt;active_forge_prompt=prompt.get();
    if(prompt->fixed_home)forge_fixed_home_render(controller,prompt,&no_arguments,true);
    else forge_prompt_render(controller,&prompt->text_id,&no_arguments,true,original_forge_prompt_init,true);
    active_forge_prompt=previous;
    if (!prompt->rendered) throw std::runtime_error("Nested crafting page did not render");
    forge_prompt_page_log(*prompt);
}
bool forge_prompt_choose(void* controller,const std::shared_ptr<ForgePrompt>& prompt,int index,int& native_answer) {
    forge_plans.erase(prompt->accessor);native_answer=1;
    if (!prompt->rendered) throw std::runtime_error("Crafting page was not rendered");
    if(prompt->blocked) {prompt->finished=true;return true;}
    if(prompt->home) {
        if(prompt->home_recipes) {
            const int count=prompt->home_recent?4:3;
            if(index==count || index<0) {prompt->home_recipes=false;forge_prompt_next_page(controller,prompt);return false;}
            if(index>=count) throw std::runtime_error("Invalid recipe category");
            camp_mode=itemization::CampMode::Quality;native_answer=index;prompt->finished=true;return true;
        }
        if(index==0) {prompt->home_recipes=true;forge_prompt_next_page(controller,prompt);return false;}
        if(index<0 || index==4) {native_answer=-1;prompt->finished=true;return true;}
        if(index==2 || index==3) {craft_screen_open(index==3,0,reinterpret_cast<uintptr_t>(controller));return false;}
        if(index!=1) throw std::runtime_error("Invalid campsite action");
        camp_mode=itemization::CampMode::Quality;native_answer=prompt->home_recent?4:3;prompt->finished=true;return true;
    }
    if(prompt->quality_confirm) {
        const auto accessor=reinterpret_cast<void*>(prompt->accessor);
        const int cost=forge_base_pearl_cost(accessor,prompt->source);
        native_answer=index==0 && owned_count(find_manager(),prompt->source)==1 && original_forge_pearls(accessor,0)>=cost?0:1;
        camp_mode=itemization::CampMode::Quality;prompt->finished=true;return true;
    }
    if(prompt->camp_menu) {
        const auto selection=prompt->camp_menu->select(index);
        if(selection.apply) {
            std::string result;
            try {
                if(!selection.shard.empty()) {
                    const auto item=shard_commit(prompt->accessor,prompt->source,selection.shard);
                    result=itemization::instance_name(item)+"\r\n"+camp_policy->definition(selection.shard).at("name").get<std::string>()+" applied.";
                } else {
                    const auto outcome=salvage_commit(selection.salvage);const auto& drops=outcome.at("shards");result="Salvaged "+outcome.at("salvaged").dump()+" items.\r\n";
                    if(outcome.at("skipped_locked").get<int>())result+="Skipped "+outcome.at("skipped_locked").dump()+" locked items.\r\n";
                    if(drops.empty()) result+="No shards recovered.";
                    else {
                        size_t shown=0;for(auto q=drops.begin();q!=drops.end() && shown<6;++q,++shown)
                            result+=camp_policy->definition(q.key()).at("name").get<std::string>()+" x"+std::to_string(q.value().get<int>())+"\r\n";
                        if(drops.size()>shown) result+="More shard types added. View them in inventory.";
                    }
                }
            } catch(const std::exception& error) {log({{"event","camp_craft_refused"},{"reason",error.what()}});result="The craft could not be completed. Return to equipment and check the selection.";}
            prompt->camp_menu->show_result(result);forge_prompt_next_page(controller,prompt);return false;
        }
        if(!selection.finished) {forge_prompt_next_page(controller,prompt);return false;}
        prompt->finished=true;return true;
    }
    if(prompt->recipe) {
        if(!prompt->recipe_applied && index==0) {
            // Latch before committing so repeated UI callbacks cannot create twice.
            prompt->recipe_applied=true;
            const auto created=veyl_recipe_commit(prompt->accessor,prompt->recipe);
            prompt->text="Created "+itemization::instance_name(created)+".\r\nUse Veyls to change its modifiers.<select>Return to recipes</select><end>";
            forge_prompt_next_page(controller,prompt);return false;
        }
        prompt->finished=true;return true;
    }
    if(prompt->veyl_menu) {
        const auto selection=prompt->veyl_menu->select(index);
        if(selection.choice) {
            const auto changed=veyl_commit(prompt->accessor,prompt->source,*selection.choice,selection.cost);
            prompt->veyl_menu->show_result(itemization::instance_name(changed)+"\r\n"+
                veyl_policy->definition(selection.choice->currency).at("name").get<std::string>()+" applied.\r\n"+
                "View the updated modifiers in the equipment list.");
            forge_prompt_next_page(controller,prompt);return false;
        }
        if(!selection.finished) {forge_prompt_next_page(controller,prompt);return false;}
        prompt->finished=true;return true; // Native No resumes equipment focus, never starts the minigame.
    }
    const auto selection=prompt->menu->select(index);
    if (!selection.finished) {forge_prompt_next_page(controller,prompt);return false;}
    prompt->finished=true;
    if (selection.choice) {
        const auto& choice=*selection.choice;
        const auto item=item_service->lookup(runtime_ids.at(prompt->source));
        const auto cost=forge_policy->cost(original_forge_cost(reinterpret_cast<void*>(prompt->accessor),prompt->source),choice.operation);
        if (owned_count(find_manager(),prompt->source)==1 && cost==selection.cost &&
            original_forge_pearls(reinterpret_cast<void*>(prompt->accessor),0)>=cost &&
            forge_policy->can_apply(*item_service,item,choice)) {
            forge_plans.emplace(prompt->accessor,ForgePlan{prompt->source,choice,cost});native_answer=0;
            log({{"event","targeted_forge_choice"},{"engine_id",item.at("engine_id")},
                {"operation",choice.operation},{"target_affix",choice.affix_id},
                {"chosen_replacement",choice.replacement_id},{"pearl_cost",cost}});
        }
    }
    if (native_answer==1) log({{"event","targeted_forge_cancelled"}});
    return true;
}
void forge_prompt_control_hook(void* controller,uint64_t event,void* selected) {
    if(craft_screen_is_open()) return;
    ForgeMenuTiming timing("selection");
    bool translate=false;int native_answer=1;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=forge_prompts.find(reinterpret_cast<uintptr_t>(controller));
        if (found!=forge_prompts.end() && !found->second->finished && selected && event==read<uint64_t>(base+0x5C1E3B0)) {
            const auto prompt=found->second;translate=true;
            timing.active=true;
            const int index=read<int32_t>(reinterpret_cast<uintptr_t>(selected)+0x3BC);
            if (!forge_prompt_choose(controller,prompt,index,native_answer)) return;
        }
    } catch (const std::exception& error) {log({{"event","targeted_forge_choice_refused"},{"reason",error.what()}});}
    const auto previous_controller=forge_completion_controller;const auto previous_answer=forge_completion_answer;const auto previous_focus=forge_completion_return_focus;
    if(translate) {forge_completion_controller=reinterpret_cast<uintptr_t>(controller);forge_completion_answer=native_answer;
        const auto p=forge_prompts.at(reinterpret_cast<uintptr_t>(controller));forge_completion_return_focus=!p->home;}
    original_forge_prompt_control(controller,event,selected);
    forge_completion_controller=previous_controller;forge_completion_answer=previous_answer;forge_completion_return_focus=previous_focus;
    if (translate) *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(controller)+0x344)=native_answer;
}
void forge_prompt_widget_hook(void* controller,uint64_t event) {
    if(craft_screen_is_open()) return;
    ForgeMenuTiming timing("back");
    bool translate=false;int native_answer=1;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        auto found=forge_prompts.find(reinterpret_cast<uintptr_t>(controller));
        if (found!=forge_prompts.end() && !found->second->finished &&
            read<uintptr_t>(reinterpret_cast<uintptr_t>(controller)+0x278)!=0 && event==read<uint64_t>(base+0x5C1E358)) {
            const auto prompt=found->second;translate=true;
            timing.active=true;
            int back=0;
            if(prompt->home) back=prompt->home_recipes?(prompt->home_recent?4:3):4;
            else if(prompt->quality_confirm) back=1;
            else if(prompt->camp_menu) {
                const auto rows=prompt->camp_menu->rows();back=static_cast<int>(rows.size())-1;
                for(size_t i=0;i<rows.size();++i) if(rows[i].key=="back") back=static_cast<int>(i);
            } else if(prompt->veyl_menu) {
                const auto rows=prompt->veyl_menu->rows();back=static_cast<int>(rows.size())-1;
                for(size_t i=0;i<rows.size();++i) if(rows[i].key=="back") back=static_cast<int>(i);
            } else if(prompt->menu) {
                const auto rows=prompt->menu->rows();back=static_cast<int>(rows.size())-1;
                for(size_t i=0;i<rows.size();++i) if(rows[i].key=="back") back=static_cast<int>(i);
            } else back=prompt->recipe_applied?0:1;
            // Native B/back returns -1, but the original Blueprint only handles
            // Yes=0/No=1. Navigate within the menu or return an explicit No.
            if (!forge_prompt_choose(controller,prompt,back,native_answer)) return;
        }
    } catch (const std::exception& error) {log({{"event","targeted_forge_back_refused"},{"reason",error.what()}});}
    const auto previous_controller=forge_completion_controller;const auto previous_answer=forge_completion_answer;const auto previous_focus=forge_completion_return_focus;
    if(translate) {forge_completion_controller=reinterpret_cast<uintptr_t>(controller);forge_completion_answer=native_answer;
        const auto p=forge_prompts.at(reinterpret_cast<uintptr_t>(controller));forge_completion_return_focus=!p->home;}
    original_forge_prompt_widget(controller,event);
    forge_completion_controller=previous_controller;forge_completion_answer=previous_answer;forge_completion_return_focus=previous_focus;
    if (translate) *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(controller)+0x344)=native_answer;
}
