// Read-only performance feedback on the native minigame's game-thread update.
// New widgets belong to the live WidgetTree and Canvas slots (engine GC owns
// them). Cached references use object index + serial, never unguarded pointers.
using ForgeGaugeUpdateFn=float(*)(void*,int,float);
ForgeGaugeUpdateFn original_forge_gauge_update=nullptr;
using FeedbackEventFn=void(*)(void*,void*,void*);
using FeedbackConstructFn=uintptr_t(*)(uintptr_t,uintptr_t,uint64_t,uint32_t,uint32_t,uintptr_t,bool,uintptr_t,bool);
using FeedbackTextFn=void*(*)(void*,const Array*);
struct FeedbackWeak {
    int index=-1,serial=0;
    static FeedbackWeak from(uintptr_t object) {
        const int index=itemization::native_object_slot(read<uint32_t>(object+0xC));
        const auto count=read<int32_t>(base+0x5D83BF8+0x1C);
        if(index<0 || index>=count) throw std::runtime_error("Feedback object index invalid");
        const auto entry=read<uintptr_t>(base+0x5D83BF8+0x10)+uintptr_t(index)*24;
        if(read<uintptr_t>(entry)!=object) throw std::runtime_error("Feedback object not registered");
        return {index,read<int32_t>(entry+0x10)};
    }
    uintptr_t get(bool allow_default=false) const {
        if(index<0 || index>=read<int32_t>(base+0x5D83BF8+0x1C)) return 0;
        const auto entry=read<uintptr_t>(base+0x5D83BF8+0x10)+uintptr_t(index)*24;
        if(read<int32_t>(entry+0x10)!=serial || read<uint32_t>(entry+8)&((1U<<28)|(1U<<29))) return 0;
        const auto object=read<uintptr_t>(entry);
        return object && !(read<uint32_t>(object+8)&((allow_default?0U:0x30U)|0x8000U|0x10000U))?object:0;
    }
};
struct FeedbackGaugeUI {
    FeedbackWeak red_left,red_right,yellow_left,yellow_right,green,score;
    std::array<double,6> previous_geometry{};
};
struct FeedbackUI {
    FeedbackWeak owner,card,title,score,goals;
    std::array<FeedbackGaugeUI,8> gauges;
    std::array<std::string,8> previous_gauges;
    std::string previous_title,previous_score,previous_goals;
    uint64_t previous_log=0;
    float previous_card_y=-1;
};
std::map<std::pair<int,int>,FeedbackUI> forge_feedback_uis;
std::map<std::string,uintptr_t> feedback_functions;
uintptr_t feedback_image_class=0,feedback_text_class=0,feedback_canvas_class=0;
bool forge_feedback_enabled=true;
void feedback_bind_umg(uintptr_t text,uintptr_t image,uintptr_t canvas);
thread_local const char* feedback_stage="idle";
const std::array<std::pair<uintptr_t,std::array<uint8_t,5>>,4>& forge_feedback_bindings() {
    static const std::array<std::pair<uintptr_t,std::array<uint8_t,5>>,4> bindings{{
        {0x934AD0,{0x48,0x63,0xC2,0x0F,0x57}},
        {0xEF12A0,{0x40,0x55,0x56,0x57,0x41}},
        {0xF16220,{0x4C,0x89,0x44,0x24,0x18}},
        {0x1B0AA30,{0x40,0x53,0x48,0x83,0xEC}}}};
    return bindings;
}
bool feedback_is_a(uintptr_t object,const char* wanted) {
    auto cls=read<uintptr_t>(object+0x10);
    for(int depth=0;cls && depth<32;++depth) {
        if(fname(read<uint64_t>(cls+0x18))==wanted) return true;
        cls=read<uintptr_t>(cls+0x40);
    }
    return false;
}
void feedback_event(uintptr_t object,const char* function,void* params) {
    reinterpret_cast<FeedbackEventFn>(base+0xEF12A0)(reinterpret_cast<void*>(object),
        reinterpret_cast<void*>(feedback_functions.at(function)),params);
}
void feedback_visibility(uintptr_t object,uint8_t value) {feedback_event(object,"Widget.SetVisibility",&value);}
void feedback_rect(uintptr_t object,float x,float y,float width,float height) {
    const auto slot=read<uintptr_t>(object+0x38);
    if(!slot || !feedback_is_a(slot,"CanvasPanelSlot")) throw std::runtime_error("Feedback canvas slot missing");
    const std::array<float,10> layout{x,y,width,height,0,0,0,0,0,0};
    original_canvas_layout(reinterpret_cast<void*>(slot),layout.data());
}
uintptr_t feedback_widget(uintptr_t tree,uintptr_t cls,uintptr_t parent,const std::string& label,uintptr_t text_style=0) {
    const auto object=reinterpret_cast<FeedbackConstructFn>(base+0xF16220)(cls,tree,native_name(label),0,0,0,false,0,false);
    if(!object || read<uintptr_t>(object+0x20)!=tree) throw std::runtime_error("Feedback widget construction failed");
    if(text_style) {
        // Native plain-value styling, before the widget's first Slate build.
        // Font is applied with SetFont below; never memcpy shared font/color refs.
        std::memcpy(reinterpret_cast<void*>(object+0x1B0),reinterpret_cast<void*>(text_style+0x1B0),24);
        std::memcpy(reinterpret_cast<void*>(object+0x1E0),reinterpret_cast<void*>(text_style+0x1E0),16);
        std::memcpy(reinterpret_cast<void*>(object+0x200),reinterpret_cast<void*>(text_style+0x200),8);
    }
    struct {uintptr_t content,result;} params{object,0};
    feedback_event(parent,"CanvasPanel.AddChildToCanvas",&params);
    if(!params.result || read<uintptr_t>(object+0x38)!=params.result) throw std::runtime_error("Feedback widget attachment failed");
    feedback_visibility(object,3); // Render, but never participate in input/focus.
    return object;
}
uintptr_t feedback_image(uintptr_t tree,uintptr_t parent,const std::string& label,const std::array<float,4>& colour) {
    const auto image=feedback_widget(tree,feedback_image_class,parent,label);
    // Default UImage brush is the engine's white primitive. Tinting it creates
    // a translucent rectangle without replacing the chevron texture beneath.
    feedback_event(image,"Image.SetColorAndOpacity",const_cast<float*>(colour.data()));
    return image;
}
uintptr_t feedback_text(uintptr_t tree,uintptr_t parent,const std::string& label,uintptr_t font_source,int size) {
    const auto text=feedback_widget(tree,feedback_text_class,parent,label,font_source);
    std::array<uint8_t,0x58> font{};std::memcpy(font.data(),reinterpret_cast<void*>(font_source+0x158),font.size());
    std::memcpy(font.data()+0x50,&size,sizeof(size));
    feedback_event(text,"TextBlock.SetFont",font.data());
    uint8_t centre=1;feedback_event(text,"TextBlock.SetJustification",&centre);
    return text;
}
void feedback_set_text(uintptr_t widget,const std::string& text) {
    std::wstring wide;wide.reserve(text.size());for(const auto c:text) wide.push_back(static_cast<unsigned char>(c));
    Array string{reinterpret_cast<uintptr_t>(wide.c_str()),static_cast<int>(wide.size()+1),static_cast<int>(wide.size()+1)};
    std::array<uintptr_t,3> value{};
    reinterpret_cast<FeedbackTextFn>(base+0x1B0AA30)(value.data(),&string);
    feedback_event(widget,"TextBlock.SetText",value.data());
    // Same thread-safe shared-reference release emitted by the verified engine
    // text conversion / SetText thunks. Slate retains its own FText reference.
    if(const auto reference=value[1]) {
        if(InterlockedDecrement(reinterpret_cast<volatile LONG*>(reference+8))==0) {
            const auto vtable=read<uintptr_t>(reference);
            reinterpret_cast<void(*)(void*)>(read<uintptr_t>(vtable))(reinterpret_cast<void*>(reference));
            if(InterlockedDecrement(reinterpret_cast<volatile LONG*>(reference+12))==0)
                reinterpret_cast<void(*)(void*,uint32_t)>(read<uintptr_t>(vtable+8))(reinterpret_cast<void*>(reference),1);
        }
    }
}
uintptr_t feedback_find_text(uintptr_t widget,int& budget) {
    if(!widget || --budget<0) return 0;
    if(feedback_is_a(widget,"TextBlock")) return widget;
    if(feedback_is_a(widget,"UserWidget")) {
        const auto tree=read<uintptr_t>(widget+0x168);
        return tree?feedback_find_text(read<uintptr_t>(tree+0x38),budget):0;
    }
    if(!feedback_is_a(widget,"PanelWidget")) return 0;
    const auto slots=read<Array>(widget+0xC8);
    if(slots.count<0 || slots.count>256 || slots.capacity<slots.count) return 0;
    for(int i=0;i<slots.count;++i) {
        const auto slot=read<uintptr_t>(slots.data+uintptr_t(i)*8);
        if(slot) if(const auto found=feedback_find_text(read<uintptr_t>(slot+0x40),budget)) return found;
    }
    return 0;
}
uintptr_t feedback_command_font(uintptr_t game_base) {
    // The forge owns its command widget directly; no whole-game object scan.
    const auto command=read<uintptr_t>(game_base+0x560);
    if(command && feedback_is_a(command,"WBP_Menu_FusigiKazi_Game_Command_000_modern_C")) {
        int budget=1024;if(const auto font=feedback_find_text(command,budget)) return font;
    }
    throw std::runtime_error("Feedback native command font missing");
}
void feedback_require_property(uintptr_t cls,const std::string& property,int offset) {
    for(int depth=0;cls && depth<32;++depth,cls=read<uintptr_t>(cls+0x40)) {
    auto field=read<uintptr_t>(cls+0x48);
    // Owner build UObject is 0x38 bytes: UField.Next is +0x38, not +0x28.
    for(int budget=2048;field && budget>0;--budget,field=read<uintptr_t>(field+0x38)) {
        if(fname(read<uint64_t>(field+0x18))==property && feedback_is_a(field,"Property") && read<int32_t>(field+0x54)==offset) return;
    }
    }
    throw std::runtime_error("Feedback reflected property mismatch: "+property);
}
FeedbackUI& feedback_create(uintptr_t game_base,uintptr_t accessor) {
    const auto owner=FeedbackWeak::from(game_base);const auto key=std::make_pair(owner.index,owner.serial);
    if(auto found=forge_feedback_uis.find(key);found!=forge_feedback_uis.end()) return found->second;
    const auto setup_started=GetTickCount64();
    const auto cls=read<uintptr_t>(game_base+0x10);
    feedback_stage="verify game-base properties";
    feedback_require_property(cls,"Gauge_01",0x410);feedback_require_property(cls,"KaziAccessor",0x550);
    feedback_require_property(cls,"KaziCommandWidget",0x560);feedback_require_property(cls,"SuccessPoint_01",0x4B0);
    if(read<uintptr_t>(game_base+0x550)!=accessor) throw std::runtime_error("Feedback accessor owner mismatch");
    const auto tree=read<uintptr_t>(game_base+0x168);
    feedback_stage="find native font";
    const auto font=feedback_command_font(game_base);
    const auto text_cls=read<uintptr_t>(font+0x10);
    feedback_require_property(text_cls,"Font",0x158);
    feedback_require_property(text_cls,"ShadowOffset",0x1B0);
    feedback_require_property(text_cls,"ShadowColorAndOpacity",0x1B8);
    feedback_require_property(text_cls,"BorderColorAndOpacity",0x1E0);
    feedback_require_property(text_cls,"BorderWidth",0x200);
    feedback_require_property(text_cls,"CharacterSpacing",0x204);
    const auto first_gauge=read<uintptr_t>(game_base+0x410);
    const auto first_hud=read<uintptr_t>(first_gauge+0x378);
    feedback_require_property(read<uintptr_t>(first_hud+0x10),"Frame_Normal",0x3D8);
    const auto first_back=read<uintptr_t>(first_hud+0x398);
    feedback_stage="bind loaded forge UI";
    if(feedback_functions.empty()) feedback_bind_umg(font,read<uintptr_t>(first_hud+0x3D8),first_back);
    const auto gauge_slot=read<uintptr_t>(first_gauge+0x38);
    const auto panel=read<uintptr_t>(gauge_slot+0x38);
    if(!feedback_is_a(panel,"CanvasPanel")) throw std::runtime_error("Feedback gauge canvas missing");
    FeedbackUI ui;ui.owner=owner;
    feedback_stage="construct feedback card";
    const auto card=feedback_widget(tree,feedback_canvas_class,panel,"ARPG_ForgeFeedbackCard");ui.card=FeedbackWeak::from(card);
    const auto title=feedback_text(tree,card,"ARPG_ForgeFeedbackTitle",font,16);ui.title=FeedbackWeak::from(title);
    const auto score=feedback_text(tree,card,"ARPG_ForgeFeedbackScore",font,18);ui.score=FeedbackWeak::from(score);
    const auto goals=feedback_text(tree,card,"ARPG_ForgeFeedbackGoals",font,18);ui.goals=FeedbackWeak::from(goals);
    feedback_rect(title,0,0,900,32);feedback_rect(score,0,34,420,36);feedback_rect(goals,430,34,470,36);
    for(int i=0;i<8;++i) {
        feedback_stage="construct gauge bands";
        const auto wrapper=read<uintptr_t>(game_base+0x410+uintptr_t(i)*8);
        if(!wrapper || !feedback_is_a(wrapper,"WBP_Menu_FusigiKazi_Game_Gauge_000_modern_C")) throw std::runtime_error("Feedback gauge wrapper mismatch");
        feedback_require_property(read<uintptr_t>(wrapper+0x10),"WBP_Hud_FusigiKazi_Gauge",0x378);
        const auto hud=read<uintptr_t>(wrapper+0x378);
        feedback_require_property(read<uintptr_t>(hud+0x10),"BackPanel",0x398);
        feedback_require_property(read<uintptr_t>(hud+0x10),"SeikouBack",0x418);
        const auto back_panel=read<uintptr_t>(hud+0x398);
        const auto hud_tree=read<uintptr_t>(hud+0x168);
        const auto prefix="ARPG_ForgeGauge"+std::to_string(i);
        auto& gauge=ui.gauges[i];
        gauge.red_left=FeedbackWeak::from(feedback_image(hud_tree,back_panel,prefix+"RedLeft",{0.78f,0.06f,0.025f,0.48f}));
        gauge.red_right=FeedbackWeak::from(feedback_image(hud_tree,back_panel,prefix+"RedRight",{0.78f,0.06f,0.025f,0.48f}));
        gauge.yellow_left=FeedbackWeak::from(feedback_image(hud_tree,back_panel,prefix+"YellowLeft",{0.95f,0.61f,0.035f,0.56f}));
        gauge.yellow_right=FeedbackWeak::from(feedback_image(hud_tree,back_panel,prefix+"YellowRight",{0.95f,0.61f,0.035f,0.56f}));
        gauge.green=FeedbackWeak::from(feedback_image(hud_tree,back_panel,prefix+"Green",{0.08f,0.78f,0.17f,0.60f}));
        const auto wrapper_tree=read<uintptr_t>(wrapper+0x168);
        const auto wrapper_root=read<uintptr_t>(wrapper_tree+0x38);
        if(!feedback_is_a(wrapper_root,"CanvasPanel")) throw std::runtime_error("Feedback score canvas missing");
        const auto label=feedback_text(wrapper_tree,wrapper_root,prefix+"Score",font,16);gauge.score=FeedbackWeak::from(label);
        feedback_rect(label,0,66,336,26);
    }
    // Remove dead weak state when a world/forge WidgetTree has been discarded.
    for(auto it=forge_feedback_uis.begin();it!=forge_feedback_uis.end();) {
        if(!it->second.owner.get()) it=forge_feedback_uis.erase(it);else ++it;
    }
    log({{"event","forge_feedback_created"},{"owner_index",owner.index},{"owner_serial",owner.serial},
        {"transparent_bands",true},{"native_diamond_unchanged",true},{"game_state",read<uint8_t>(accessor+0x44)},
        {"setup_ms",GetTickCount64()-setup_started},{"whole_game_object_scans",0}});
    return forge_feedback_uis.emplace(key,std::move(ui)).first->second;
}
void forge_feedback_update(void* accessor) {
    feedback_stage="validate live session";
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    const auto address=reinterpret_cast<uintptr_t>(accessor);
    const auto session=forge_sessions.find(address);
    if(!runtime_enabled.load() || !forge_feedback_enabled || !forge_policy || !forge_policy->uses_performance() ||
        session==forge_sessions.end() || (!camp_policy && (!session->second.modded || !session->second.retry)) || session->second.committed ||
        read<uint8_t>(address+0x44)>3 || (session->second.retry && read<uint64_t>(address+0x430)!=session->second.old_id)) return;
    const auto game_base=read<uintptr_t>(address+0x110);
    if(!game_base || !feedback_is_a(game_base,"WBP_UMG_FusigiKazi_Game_Base_000_modern_C")) return;
    if(!read<uintptr_t>(game_base+0x560)) return;
    // Use the first ready gauge callback, including the native opening state.
    // All active native HUDs must already match the new gauge parameters.
    for(int i=0;i<8;++i) {
        const auto record=address+0x24C+uintptr_t(i)*0x28;
        const auto maximum=read<float>(record+0x10);if(maximum<=0) continue;
        const auto wrapper=read<uintptr_t>(game_base+0x410+uintptr_t(i)*8);if(!wrapper) return;
        const auto hud=read<uintptr_t>(wrapper+0x378);if(!hud || !read<uintptr_t>(game_base+0x4B0+uintptr_t(i)*8)) return;
        if(read<float>(hud+0x434)!=read<float>(record) || read<float>(hud+0x438)!=read<float>(record+4) ||
           read<float>(hud+0x43C)!=maximum) return;
        const auto point=read<uintptr_t>(game_base+0x4B0+uintptr_t(i)*8),back=read<uintptr_t>(hud+0x398),root=read<uintptr_t>(hud+0x400);
        if(!back || !root) return;
        const auto point_slot=read<uintptr_t>(point+0x38),back_slot=read<uintptr_t>(back+0x38),root_slot=read<uintptr_t>(root+0x38),wrapper_slot=read<uintptr_t>(wrapper+0x38);
        if(!point_slot || !back_slot || !root_slot || !wrapper_slot) return;
        const float point_x=read<float>(point_slot+0x48)+read<float>(point_slot+0x50)*0.5f-read<float>(wrapper_slot+0x48);
        const float diamond=read<float>(root+0x68)<0?read<float>(root_slot+0x50)-read<float>(back_slot+0x48)-point_x:point_x-read<float>(back_slot+0x48);
        if(read<float>(point_slot+0x4C)<=55 || diamond<0 || diamond>read<float>(back_slot+0x50)) return;
    }
    auto& ui=feedback_create(game_base,address);
    feedback_stage="update feedback geometry";
    std::vector<itemization::ForgeGauge> gauges;float last_score_bottom=0;
    for(int i=0;i<8;++i) {
        const auto record=address+0x24C+uintptr_t(i)*0x28;
        const auto maximum=read<float>(record+0x10);
        auto& widgets=ui.gauges[i];
        if(maximum<=0) {if(const auto text=widgets.score.get()) feedback_visibility(text,1);continue;}
        const itemization::ForgeGauge gauge{read<float>(record),read<float>(record+4),read<float>(record+8),read<float>(record+0xC)};
        gauges.push_back(gauge);
        const auto wrapper=read<uintptr_t>(game_base+0x410+uintptr_t(i)*8);
        const auto hud=read<uintptr_t>(wrapper+0x378);
        const auto back_panel=read<uintptr_t>(hud+0x398);
        const auto back_slot=read<uintptr_t>(back_panel+0x38);
        const float width=read<float>(back_slot+0x50);
        const auto wrapper_slot=read<uintptr_t>(wrapper+0x38);
        const auto point=read<uintptr_t>(game_base+0x4B0+uintptr_t(i)*8);
        const auto point_slot=read<uintptr_t>(point+0x38);
        const auto root=read<uintptr_t>(hud+0x400);
        const auto root_slot=read<uintptr_t>(root+0x38);
        const bool reverse=read<float>(root+0x68)<0;
        const float point_x=read<float>(point_slot+0x48)+read<float>(point_slot+0x50)*0.5f-read<float>(wrapper_slot+0x48);
        const float root_width=read<float>(root_slot+0x50),back_x=read<float>(back_slot+0x48);
        const float diamond=reverse?root_width-back_x-point_x:point_x-back_x;
        const auto bands=itemization::forge_feedback_pixels(gauge,maximum,width,diamond);
        if(width>400)
            throw std::runtime_error("Feedback gauge width invalid");
        const auto red_left=widgets.red_left.get(),red_right=widgets.red_right.get(),yellow_left=widgets.yellow_left.get(),
            yellow_right=widgets.yellow_right.get(),green=widgets.green.get(),label=widgets.score.get();
        if(!red_left || !red_right || !yellow_left || !yellow_right || !green || !label) throw std::runtime_error("Feedback widget expired");
        const std::array<double,6> geometry{width,maximum,gauge.lower,gauge.upper,gauge.target,diamond};
        if(widgets.previous_geometry!=geometry) {
        feedback_rect(red_left,0,0,static_cast<float>(bands.lower),56);
        feedback_rect(red_right,static_cast<float>(bands.upper),0,static_cast<float>(width-bands.upper),56);
        feedback_rect(yellow_left,static_cast<float>(bands.lower),0,static_cast<float>(bands.excellent_lower-bands.lower),56);
        feedback_rect(yellow_right,static_cast<float>(bands.excellent_upper),0,static_cast<float>(bands.upper-bands.excellent_upper),56);
        feedback_rect(green,static_cast<float>(bands.excellent_lower),0,static_cast<float>(bands.excellent_upper-bands.excellent_lower),56);
        widgets.previous_geometry=geometry;
        }
        // Hide only the native green background; the chevron background,
        // animated fill, exact-target highlight, frame and diamond stay native.
        feedback_visibility(read<uintptr_t>(hud+0x418),2);
        feedback_visibility(label,3);
        const auto individual=forge_policy->measure({gauge}).score;
        const auto text="Score "+itemization::forge_feedback_score(individual)+" / 100";
        if(ui.previous_gauges[i]!=text) {feedback_stage="update gauge text";feedback_set_text(label,text);ui.previous_gauges[i]=text;}
        last_score_bottom=std::max(last_score_bottom,read<float>(wrapper_slot+0x4C)+92);
    }
    const auto performance=forge_policy->measure(gauges);
    if(camp_policy) {
        const std::string title=session->second.retry?"Reforge Quality":"Forge equipment";
        const std::string score="Forging score  "+itemization::forge_feedback_score(performance.score)+" / 100";
        const std::string goals="Quality: "+itemization::display_number(itemization::quality_from_score(performance.score)/100.0)+"% / 25%";
        const float card_y=last_score_bottom+8;
        if(ui.previous_card_y!=card_y) {feedback_rect(ui.card.get(),27,card_y,900,70);ui.previous_card_y=card_y;}
        if(ui.previous_title!=title) {feedback_set_text(ui.title.get(),title);ui.previous_title=title;}
        if(ui.previous_score!=score) {feedback_set_text(ui.score.get(),score);ui.previous_score=score;}
        if(ui.previous_goals!=goals) {feedback_set_text(ui.goals.get(),goals);ui.previous_goals=goals;}
        return;
    }
    const auto& item=item_service->lookup(runtime_ids.at(session->second.old_id));
    const auto& choice=session->second.choice;
    const auto decision=forge_navigation_cache().performance_decision(runtime_ids.at(session->second.old_id),choice,performance.score);
    std::string action=choice.operation=="reroll_value"?"Reforge Value":choice.operation=="replace_affix"?"Replace modifier":choice.operation=="upgrade_tier"?"Upgrade tier":"Reroll tier";
    const auto& affixes=item.at("affixes");const auto target=std::find_if(affixes.begin(),affixes.end(),[&](const Json& a){return a.at("id")==choice.affix_id;});
    if(target==affixes.end()) throw std::runtime_error("Feedback target missing");
    std::string title=action+"  |  "+itemization::display_stat_label(target->at("label"))+"  |  T"+std::to_string(item_service->affix_tier(*target).at("rank").get<int>());
    const std::string score="Forging score  "+itemization::forge_feedback_score(performance.score)+" / 100";
    std::string goals="Success: "+std::to_string(decision.success_score)+"   Perfect: "+std::to_string(decision.perfect_score);
    if(choice.operation=="reroll_value") {
        const auto projected=itemization::forge_reforged_value(item_service->reforge_tier(item,*target),target->at("value"),decision);
        const std::string unit=target->at("unit")=="percentage_points"?"%":"";
        goals="Value: +"+target->at("value").dump()+unit+" -> +"+std::to_string(projected)+unit;
    }
    if(choice.operation=="reroll_tier") title+="  |  Reachable tier: "+(decision.reachable_ranks.empty()?std::string("none"):"T"+std::to_string(decision.reachable_ranks.back()));
    const float card_y=last_score_bottom+8;
    if(ui.previous_card_y!=card_y) {feedback_rect(ui.card.get(),27,card_y,900,70);ui.previous_card_y=card_y;}
    feedback_stage="update score card";
    if(ui.previous_title!=title) {feedback_set_text(ui.title.get(),title);ui.previous_title=title;}
    if(ui.previous_score!=score) {feedback_set_text(ui.score.get(),score);ui.previous_score=score;}
    if(ui.previous_goals!=goals) {feedback_set_text(ui.goals.get(),goals);ui.previous_goals=goals;}
    if(GetTickCount64()-ui.previous_log>2000) {
        ui.previous_log=GetTickCount64();
        log({{"event","forge_feedback_measured"},{"engine_id",item.at("engine_id")},{"operation",choice.operation},
            {"decision",decision.details()},{"gauges",performance.gauges},{"card_y",card_y}});
    }
}
float forge_gauge_update_hook(void* accessor,int index,float delta) {
    const auto result=original_forge_gauge_update(accessor,index,delta);
    if((index==0 || forge_feedback_uis.empty()) && forge_feedback_enabled) {
        try {forge_feedback_update(accessor);}
        catch(const std::exception& error) {forge_feedback_enabled=false;log({{"event","forge_feedback_disabled"},{"reason",error.what()},{"stage",feedback_stage}});}
    }
    return result;
}
void feedback_bind_umg(uintptr_t text,uintptr_t image,uintptr_t canvas) {
    const std::map<std::string,std::pair<int,uintptr_t>> expected{
        {"Widget.SetVisibility",{1,0x16DD3A0}},{"CanvasPanel.AddChildToCanvas",{16,0x16C5E20}},
        {"Image.SetColorAndOpacity",{16,0x16CE070}},{"TextBlock.SetFont",{88,0x16D3CE0}},
        {"TextBlock.SetText",{24,0x16D5190}},{"TextBlock.SetJustification",{1,0x16D4200}}};
    if(!text || !image || !canvas || !feedback_is_a(text,"TextBlock") || !feedback_is_a(image,"Image") || !feedback_is_a(canvas,"CanvasPanel"))
        throw std::runtime_error("Forge feedback source classes unavailable");
    feedback_text_class=read<uintptr_t>(text+0x10);feedback_image_class=read<uintptr_t>(image+0x10);feedback_canvas_class=read<uintptr_t>(canvas+0x10);
    for(const auto initial:{feedback_text_class,feedback_image_class,feedback_canvas_class}) {
        auto cls=initial;
        for(int depth=0;cls && depth<32;++depth,cls=read<uintptr_t>(cls+0x40)) {
            auto field=read<uintptr_t>(cls+0x48);
            for(int budget=2048;field && budget>0;--budget,field=read<uintptr_t>(field+0x38)) {
                const auto key=fname(read<uint64_t>(cls+0x18))+"."+fname(read<uint64_t>(field+0x18));
                if(const auto found=expected.find(key);found!=expected.end()) {
                    if(!feedback_is_a(field,"Function") || read<uint16_t>(field+0x9E)!=found->second.first ||
                       read<uintptr_t>(field+0xC0)!=base+found->second.second)
                        throw std::runtime_error("Forge feedback reflected function mismatch");
                    feedback_functions[key]=field;
                }
            }
        }
    }
    if(!feedback_image_class || !feedback_text_class || !feedback_canvas_class || feedback_functions.size()!=expected.size())
        throw std::runtime_error("Forge feedback UMG bindings unavailable");
}
void create_forge_feedback_hook() {
    for(const auto& binding:forge_feedback_bindings()) for(size_t i=0;i<binding.second.size();++i)
        if(read<uint8_t>(base+binding.first+i)!=binding.second[i]) throw std::runtime_error("Forge feedback native binding mismatch");
    // UMG classes may not exist during launcher injection. Bind them once on
    // the first actual forge update, when its widgets are loaded and owned.
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x934AD0),reinterpret_cast<void*>(&forge_gauge_update_hook),
        reinterpret_cast<void**>(&original_forge_gauge_update))!=MH_OK) throw std::runtime_error("Forge feedback hook creation failed");
}
