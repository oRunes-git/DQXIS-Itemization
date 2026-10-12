// Independent engine-managed pages. A command-window Blueprint supplies the
// native focus/input shell only; its command contents and actions are replaced
// by our own equipment/currency or salvage canvas. No forge UI is opened.
using CraftEventFn=void(*)(void*,void*,void*);
CraftEventFn original_craft_event=nullptr;
thread_local bool craft_internal_event=false;
bool craft_constructing=false,craft_ui_enabled=true;
struct CraftMenuSample {size_t calls=0;double worst_ms=0,total_ms=0;};
std::map<std::string,CraftMenuSample> craft_menu_samples;
struct CraftMenuTiming {
    const char* step;double started=forge_menu_clock_ms();
    explicit CraftMenuTiming(const char* value):step(value) {}
    ~CraftMenuTiming() {try {
        const double elapsed=forge_menu_clock_ms()-started;auto& sample=craft_menu_samples[step];
        ++sample.calls;sample.total_ms+=elapsed;sample.worst_ms=std::max(sample.worst_ms,elapsed);
        if(elapsed>8) log({{"event","crafting_menu_slow_step"},{"step",step},{"duration_ms",elapsed}});
    } catch(...) {}}
};
struct CraftText {FeedbackWeak widget;std::string previous;};
struct CraftDetailUI {
    CraftText text;FeedbackWeak highlight;
    itemization::CraftAffixChange change=itemization::CraftAffixChange::None;
    int painted_change=-1;float painted_pulse=-1;
};
struct CraftSlotUI {FeedbackWeak icon,background,rarity;CraftText marker;uint64_t previous_icon=~uint64_t(0);std::string previous_rarity;};
struct CraftUI {
    FeedbackWeak host,manager,root,origin,origin_selected,canvas,forge_controller;
    uintptr_t host_address=0;
    bool input_verified=false;
    std::unique_ptr<itemization::CraftingScreen> model;
    std::array<CraftText,itemization::craft_currency_rows> choices,amounts;
    std::array<CraftText,6> salvage_names;
    std::array<int,6> salvage_enabled{{-1,-1,-1,-1,-1,-1}};
    std::array<FeedbackWeak,itemization::craft_currency_rows> currency_icons;
    std::array<uint64_t,itemization::craft_currency_rows> currency_icon_ids{};
    std::array<std::array<float,2>,itemization::craft_currency_rows> currency_icon_layout{};
    std::array<CraftText,2> currency_sections;
    std::array<FeedbackWeak,2> currency_separators;
    std::array<int,itemization::craft_currency_rows> currency_enabled{{-1,-1,-1,-1,-1,-1,-1,-1}};
    CraftText rarity_badge,class_badge,quality_badge,veyl_tab,shard_tab,suffix_tab;
    std::array<FeedbackWeak,3> badge_frames;
    FeedbackWeak veyl_tab_frame,shard_tab_frame,suffix_tab_frame,bag_icon,selected_rarity;
    std::string previous_selected_rarity;
    CraftText title,gear_title,currency_title,details,description,wallet;
    std::array<CraftDetailUI,8> detail_rows;
    CraftText detail_footer;
    std::map<std::string,itemization::CraftChanges> change_notes;
    std::vector<itemization::CraftDetailRow> detail_lines;
    std::string detail_lines_key,pulse_item;
    ULONGLONG pulse_started=0,next_pulse_frame=0;
    size_t detail_page=0;
    itemization::CraftInventoryGrid inventory;
    std::array<std::array<CraftSlotUI,6>,9> slots;
    std::array<FeedbackWeak,8> portraits;
    std::array<FeedbackWeak,9> portrait_frames,column_dividers;
    FeedbackWeak craft_success_sound;
    std::array<int,8> portrait_characters{{-1,-1,-1,-1,-1,-1,-1,-1}};
    FeedbackWeak selection,choice_selection,selection_arrow,item_accessor,members_accessor,selected_icon,bag_track,bag_thumb;
    uint64_t selected_icon_id=~uint64_t(0);
    std::vector<std::pair<FeedbackWeak,uint8_t>> hidden_origins;
    CraftText selected_name;
    CraftText lock_hint;
    bool can_craft=false,camera_locked=false;
    std::map<uint64_t,FeedbackWeak> icon_cache;
    std::map<std::string,itemization::CraftItemPresentation> presentations;
    std::string presentation_id;
    std::array<size_t,8> painted_focus{};
    bool focus_painted=false;
};
using CraftWindowInputFn=bool(*)(void*,const void*);
CraftWindowInputFn original_craft_window_input=nullptr;
std::unique_ptr<CraftUI> craft_ui;
struct CraftMainEntry {
    FeedbackWeak owner,row,label;
    bool layout_pending=true,styled=false;
    std::array<uint8_t,0x58> font{};
};
std::map<std::pair<int,int>,CraftMainEntry> craft_main_entries;
// A recycled command Blueprint remains a custom page until collected, including
// its closing animation. Never register vanilla commands on that retired shell.
std::set<std::pair<int,int>> craft_custom_hosts;
FeedbackWeak craft_return_origin;
bool craft_return_back_pending=false;
ULONGLONG craft_return_input_until=0;
bool craft_custom_host(uintptr_t object) {
    const auto weak=FeedbackWeak::from(object);
    return craft_custom_hosts.count({weak.index,weak.serial})!=0;
}
void craft_add_main_entry(uintptr_t owner);
FeedbackWeak craft_manager,craft_command_class;
uint32_t craft_update_name=0,craft_key_name=0,craft_control_name=0,craft_construct_name=0;
uint32_t craft_main_class_name=0;
std::array<uint32_t,4> craft_blocked_names{};
std::map<std::pair<uintptr_t,std::string>,uintptr_t> craft_functions;
uintptr_t craft_function(uintptr_t object,const std::string& wanted,int size) {
    if(!object) throw std::runtime_error("Crafting UI object unavailable: "+wanted);
    const auto wanted_name=native_name(wanted);
    for(auto cls=read<uintptr_t>(object+0x10);cls;cls=read<uintptr_t>(cls+0x40))
        for(auto f=read<uintptr_t>(cls+0x48);f;f=read<uintptr_t>(f+0x38))
            if(read<uint64_t>(f+0x18)==wanted_name && feedback_is_a(f,"Function")) {
                if(read<uint16_t>(f+0x9E)!=size) throw std::runtime_error("Crafting UI function ABI mismatch: "+wanted+" (expected "+std::to_string(size)+", native "+std::to_string(read<uint16_t>(f+0x9E))+")");
                return f;
            }
    throw std::runtime_error("Crafting UI function missing: "+wanted);
}
void craft_call(uintptr_t object,const std::string& name,void* params,int size) {
    if(!object || !original_craft_event) throw std::runtime_error("Crafting UI call is not ready: "+name);
    const auto key=std::make_pair(read<uintptr_t>(object+0x10),name);
    auto found=craft_functions.find(key);if(found==craft_functions.end()) found=craft_functions.emplace(key,craft_function(object,name,size)).first;
    const bool before=craft_internal_event;craft_internal_event=true;
    try {original_craft_event(reinterpret_cast<void*>(object),reinterpret_cast<void*>(found->second),params);}
    catch(...) {craft_internal_event=before;throw;}craft_internal_event=before;
}
void craft_prepare_call(uintptr_t object,const std::string& name,int size) {
    const auto key=std::make_pair(read<uintptr_t>(object+0x10),name);
    if(!craft_functions.count(key))craft_functions.emplace(key,craft_function(object,name,size));
}
uintptr_t craft_find_named(uintptr_t node,const char* wanted,int& budget) {
    if(!node || --budget<0) return 0;
    if(fname(read<uint64_t>(node+0x18))==wanted) return node;
    if(feedback_is_a(node,"UserWidget")) {const auto t=read<uintptr_t>(node+0x168);return t?craft_find_named(read<uintptr_t>(t+0x38),wanted,budget):0;}
    if(!feedback_is_a(node,"PanelWidget")) return 0;
    const auto slots=read<Array>(node+0xC8);if(slots.count<0 || slots.count>512 || slots.capacity<slots.count) return 0;
    for(int i=0;i<slots.count;++i) {const auto s=read<uintptr_t>(slots.data+uintptr_t(i)*8);if(s) if(auto r=craft_find_named(read<uintptr_t>(s+0x40),wanted,budget)) return r;}
    return 0;
}
uintptr_t craft_ui_manager(uintptr_t context=0) {
    if(const auto current=craft_manager.get()) return current;
    uintptr_t game_owner=context;
    if(context) {
        // A main-menu widget is owned by the current GameInstance. Resolve
        // its inventory directly rather than scanning every loaded UObject.
        for(int depth=0;game_owner && depth<8 && !manager_object_is_a(game_owner,"GameInstance");++depth)
            game_owner=read<uintptr_t>(game_owner+0x20);
        if(!game_owner || !manager_object_is_a(game_owner,"GameInstance")) throw std::runtime_error("Crafting menu has no game-instance owner");
        feedback_require_property(read<uintptr_t>(game_owner+0x10),"ItemManager",0x130);
        remember_item_manager(read<uintptr_t>(game_owner+0x130));
    } else game_owner=read<uintptr_t>(find_manager()+0x20);
    // JackGameplayStatics.GetUMGManager's audited implementation resolves the
    // current world's HUD directly. No global object search is needed.
    const auto manager=reinterpret_cast<uintptr_t(*)(uintptr_t)>(base+0x7EF460)(game_owner);
    if(!manager || !feedback_is_a(manager,"JackUMGManager") || !read<uintptr_t>(manager+0x268))throw std::runtime_error("Crafting UI manager is not ready");
    feedback_require_property(read<uintptr_t>(manager+0x10),"RootWidget",0x268);
    craft_manager=FeedbackWeak::from(manager);return manager;
}
uintptr_t craft_create_widget(uintptr_t manager,uintptr_t cls) {
    (void)manager;const auto world=read<uintptr_t>(find_manager()+0x20);
    // The implementation called by WidgetBlueprintLibrary.Create; the same
    // world/class/player arguments, without a default-library object lookup.
    const auto result=reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t)>(base+0x1678580)(world,cls,0);
    if(!result || !read<uintptr_t>(result+0x168)) throw std::runtime_error("Crafting UI widget creation failed");return result;
}
void craft_bind_style(uintptr_t command,uintptr_t manager) {
    const auto root=read<uintptr_t>(manager+0x268);
    feedback_require_property(read<uintptr_t>(root+0x10),"MenuBGImagePanel",0x398);
    feedback_require_property(read<uintptr_t>(root+0x10),"MenuBGFullPaper",0x7C0);
    const auto image=read<uintptr_t>(root+0x398),canvas=read<uintptr_t>(command+0x418);
    int budget=512;const auto font=feedback_find_text(command,budget);
    if(!font || !image || !canvas) throw std::runtime_error("Native crafting menu styling unavailable");
    if(feedback_functions.empty()) feedback_bind_umg(font,image,canvas);
    // Validate all page/focus/texture calls before adding an entry or opening
    // a page. Packed reflected parameter lengths are not C++ sizeof values.
    craft_prepare_call(image,"SetBrushFromTexture",9);
    for(const auto& call:std::map<std::string,int>{{"GetUMGWidgetInFocus",8},{"AddUMGConnectWidgetWithClass",24},
        {"RemoveUMGWidget",11},{"FocusUMGWidgetByWidgetRef",8},{"CloseMenu",1},
        {"GetAccessorMembers",8},{"GetAccessorItem",8},{"SetUIDisableCamera",2}}) craft_prepare_call(manager,call.first,call.second);
    craft_prepare_call(command,"SetItemList",16);craft_prepare_call(command,"SelectItemFromRef",8);
    craft_prepare_call(command,"SetWidgetVisible",1);craft_prepare_call(command,"AddCursorMoveSetting",24);
    craft_prepare_call(command,"SetIsFocusableWindow",1);craft_prepare_call(command,"EnableKeyEvent",1);
    craft_prepare_call(command,"SetWidgetActive",1);
    craft_prepare_call(command,"ForceLayoutPrepass",0);
    craft_prepare_call(command,"SetWidgetPosition",8);craft_prepare_call(command,"SetWidgetScale",8);
    craft_prepare_call(read<uintptr_t>(canvas+0x38),"SetAutoSize",1);
    feedback_require_property(read<uintptr_t>(command+0x10),"bEnableEnterKeyEventCalling",0x20F);
    feedback_require_property(read<uintptr_t>(command+0x10),"bEnableCancelKeyEventCalling",0x210);
    craft_command_class=FeedbackWeak::from(read<uintptr_t>(command+0x10));
}
uintptr_t craft_template_class(uintptr_t manager) {
    if(const auto cls=craft_command_class.get()) return cls;
    const auto data=read<uintptr_t>(base+0x5D83BF8+0x10);const int count=read<int32_t>(base+0x5D83BF8+0x1C);
    uintptr_t cls=0;
    for(int i=0;i<count;++i) {
        const auto entry=data+uintptr_t(i)*24;if(*reinterpret_cast<const uint32_t*>(entry+8)&((1U<<28)|(1U<<29))) continue;
        const auto object=*reinterpret_cast<const uintptr_t*>(entry);if(!object || *reinterpret_cast<const uint32_t*>(object+8)&(0x10U|0x8000U|0x10000U)) continue;
        if(*reinterpret_cast<const uint64_t*>(object+0x18)==craft_main_class_name && feedback_is_a(object,"Class")) {cls=object;break;}
    }
    if(!cls) throw std::runtime_error("Native command-window class is not loaded yet");
    // Initialize styling without joining the game's focus/page stack.
    craft_constructing=true;uintptr_t command=0;
    try {command=craft_create_widget(manager,cls);craft_bind_style(command,manager);}
    catch(...) {craft_constructing=false;throw;}craft_constructing=false;return cls;
}
std::vector<Json> craft_inventory(uintptr_t manager,bool salvage) {
    if(salvage) return salvage_inventory(manager);
    std::vector<Json> out;std::set<uint64_t> seen;
    for(int bag=0;bag<=12;++bag) {
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0 || list.count>10000 || list.capacity<list.count) throw std::runtime_error("Invalid crafting equipment inventory");
        for(int i=0;i<list.count;++i) {
            const auto payload=read<uintptr_t>(list.data+uintptr_t(i)*16);if(!payload) continue;
            const auto id=read<uint64_t>(payload+0x10);
            if(!seen.insert(id).second || !runtime_ids.count(id) || read<int32_t>(payload+0x18)!=1) continue;
            auto item=item_service->lookup(runtime_ids.at(id));item["ui_equipped"]=read<int32_t>(payload+0x20)!=0;
            item["ui_region"]=read<int32_t>(payload+0x20);
            item["ui_bag"]=bag;out.push_back(std::move(item));
        }
    }
    std::sort(out.begin(),out.end(),[](const Json& a,const Json& b) {const auto an=itemization::instance_name(a),bn=itemization::instance_name(b);return an==bn?a.at("engine_id")<b.at("engine_id"):an<bn;});return out;
}
std::vector<std::string> craft_wrapped(const std::string& source,size_t width) {
    std::vector<std::string> lines;std::istringstream input(source);std::string line;
    while(std::getline(input,line)) {
        while(line.size()>width) {auto at=line.rfind(' ',width);if(at==std::string::npos || at<width/3) at=width;
            lines.push_back(line.substr(0,at));line.erase(0,at+(line[at]==' '?1:0));}
        lines.push_back(line);
    }return lines;
}
std::string craft_page_text(const std::string& text,size_t width,size_t page,size_t count) {
    const auto lines=craft_wrapped(text,width);const size_t pages=std::max(size_t(1),(lines.size()+count-1)/count),start=(page%pages)*count;
    std::string out;for(size_t i=start;i<std::min(lines.size(),start+count);++i) {if(i>start) out+='\n';out+=lines[i];}
    if(lines.size()>count) out+="\n[X] More  "+std::to_string(start/count+1)+" / "+std::to_string((lines.size()+count-1)/count);return out;
}
void craft_text_set(CraftText& text,const std::string& value) {
    if(text.previous==value) return;const auto w=text.widget.get();if(!w) throw std::runtime_error("Crafting text expired");
    feedback_set_text(w,value);text.previous=value;
}
CraftText craft_text_create(uintptr_t tree,uintptr_t canvas,uintptr_t font,const std::string& name,float x,float y,float w,float h,int size) {
    const auto t=feedback_text(tree,canvas,name,font,size);uint8_t left=0;feedback_event(t,"TextBlock.SetJustification",&left);
    feedback_rect(t,x,y,w,h);return {FeedbackWeak::from(t),{}};
}
#include "crafting_inventory_ui.hpp"
#include "inventory_rarity_ui.hpp"
#include "equipment_lock_ui.hpp"
void craft_details_prepare(CraftUI& ui) {
    const auto id=ui.inventory.selected().id;
    if(ui.detail_lines_key==id&&!ui.detail_lines.empty())return;
    ui.detail_lines_key=id;ui.detail_lines.clear();
    const auto generated=id.empty()?runtime_ids.end():runtime_ids.find(native_name(id));
    if(generated!=runtime_ids.end()) {
        const auto& item=item_service->lookup(generated->second);
        const auto note=ui.change_notes.find(item.at("instance_id").get<std::string>());
        ui.detail_lines=itemization::craft_wrap_details(itemization::craft_changed_details(*item_service,item,
            note==ui.change_notes.end()?nullptr:&note->second));
    }else for(const auto& line:craft_wrapped(craft_selected_presentation(ui).details,76))ui.detail_lines.push_back({line,{}});
}
void craft_details_colours(CraftUI& ui,float pulse) {
    for(auto& row:ui.detail_rows) {
        const int change=static_cast<int>(row.change);
        const float light=row.change==itemization::CraftAffixChange::None?0:pulse;
        if(change==row.painted_change&&std::abs(light-row.painted_pulse)<0.002f)continue;
        row.painted_change=change;row.painted_pulse=light;
        const bool marked=row.change!=itemization::CraftAffixChange::None;
        const std::array<float,4> background{1.0f,0.82f,0.05f,0.28f+light*0.35f};
        craft_call(row.highlight.get(),"SetColorAndOpacity",const_cast<float*>(background.data()),16);
        std::array<uint8_t,40> colour{};
        const std::array<float,4> tint=marked?std::array<float,4>{0.26f+light*0.31f,0.11f+light*0.18f,0.025f,1}:
            std::array<float,4>{0.16f,0.075f,0.035f,1};
        std::memcpy(colour.data(),tint.data(),16);craft_call(row.text.widget.get(),"SetColorAndOpacity",colour.data(),40);
    }
}
void craft_details_render(CraftUI& ui) {
    const bool salvage=ui.model->kind()==itemization::CraftScreenKind::Salvage;
    feedback_visibility(ui.details.widget.get(),salvage?3:1);
    if(salvage) {
        craft_text_set(ui.details,ui.inventory.selected().id.empty()?"No equipment in the Equipment Bag.":
            craft_page_text(craft_selected_presentation(ui).details,76,ui.detail_page,8));
    }else craft_details_prepare(ui);
    const size_t pages=std::max(size_t(1),(ui.detail_lines.size()+7)/8),first=(ui.detail_page%pages)*8;
    for(size_t i=0;i<ui.detail_rows.size();++i) {
        auto& row=ui.detail_rows[i];const auto line=!salvage&&first+i<ui.detail_lines.size()?&ui.detail_lines[first+i]:nullptr;
        row.change=line?line->change:itemization::CraftAffixChange::None;
        const bool marked=line&&row.change!=itemization::CraftAffixChange::None;
        feedback_visibility(row.text.widget.get(),line?3:1);
        feedback_visibility(row.highlight.get(),marked?3:1);
        craft_text_set(row.text,line?line->text:"");
    }
    feedback_visibility(ui.detail_footer.widget.get(),!salvage&&pages>1?3:1);
    craft_text_set(ui.detail_footer,!salvage&&pages>1?"[X] More  "+std::to_string(first/8+1)+" / "+std::to_string(pages):"");
    const auto now=GetTickCount64();
    const float pulse=ui.pulse_started&&ui.pulse_item==ui.inventory.selected().id?
        itemization::craft_change_pulse(static_cast<double>(now-ui.pulse_started)):0;
    craft_details_colours(ui,pulse);
}
void craft_change_tick() {
    if(!craft_ui||!craft_ui->pulse_started)return;auto& ui=*craft_ui;
    const auto now=GetTickCount64();if(now<ui.next_pulse_frame)return;ui.next_pulse_frame=now+30;
    const auto elapsed=static_cast<double>(now-ui.pulse_started);
    craft_details_colours(ui,ui.pulse_item==ui.inventory.selected().id?itemization::craft_change_pulse(elapsed):0);
    if(elapsed>=900)ui.pulse_started=0;
}
void craft_render() {
    if(!craft_ui || !craft_ui->host.get()) return;auto& ui=*craft_ui;const auto& m=*ui.model;
    const auto lock_id=ui.inventory.selected().id;
    const bool lockable=!lock_id.empty()&&runtime_ids.count(native_name(lock_id));
    const bool locked=lockable&&item_service->is_locked(lock_id);
    craft_text_set(ui.lock_hint,lockable&&m.stage()==itemization::CraftScreenStage::Equipment?
        std::string(locked?"Locked against salvage":""):"");
    CraftMenuTiming timing("render");
    const bool salvage=m.kind()==itemization::CraftScreenKind::Salvage;
    craft_text_set(ui.title,salvage?"Salvaging":"Crafting");
    craft_grid_render(ui);
    craft_text_set(ui.currency_title,salvage?"Salvage selection":"");
    feedback_visibility(ui.veyl_tab.widget.get(),salvage?1:3);feedback_visibility(ui.shard_tab.widget.get(),salvage?1:3);feedback_visibility(ui.suffix_tab.widget.get(),salvage?1:3);
    feedback_visibility(ui.veyl_tab_frame.get(),!salvage&&!m.shard_tab()?3:1);
    feedback_visibility(ui.shard_tab_frame.get(),!salvage&&m.currency_tab()==itemization::CraftCurrencyTab::Prefixes?3:1);
    feedback_visibility(ui.suffix_tab_frame.get(),!salvage&&m.currency_tab()==itemization::CraftCurrencyTab::Suffixes?3:1);
    const auto& rows=m.choices();
    const auto layout=itemization::craft_currency_layout(rows,m.choice_cursor(),!salvage&&m.shard_tab()&&
        (m.stage()==itemization::CraftScreenStage::Equipment||m.stage()==itemization::CraftScreenStage::Currency));
    const size_t cf=layout.first;
    for(size_t s=0;s<layout.sections.size();++s) {
        const auto& section=layout.sections[s];
        feedback_visibility(ui.currency_sections[s].widget.get(),section.visible?3:1);
        feedback_visibility(ui.currency_separators[s].get(),section.visible?3:1);
        if(section.visible) {
            craft_text_set(ui.currency_sections[s],section.eligible?"Eligible":"Ineligible");
            feedback_rect(ui.currency_sections[s].widget.get(),1224,section.y,622,28);
            feedback_rect(ui.currency_separators[s].get(),1218,section.y+28,626,2);
        }
    }
    for(size_t i=0;i<itemization::craft_currency_rows;++i) {
        const auto row=cf+i<rows.size()?&rows[cf+i]:nullptr;
        craft_text_set(ui.choices[i],row?row->label:"");craft_text_set(ui.amounts[i],row&&!row->native_id.empty()?std::to_string(row->amount):"");
        const int available=row&&row->available&&(salvage||ui.can_craft)?1:0;
        if(available!=ui.currency_enabled[i]){craft_available_text(ui.choices[i],available);craft_available_text(ui.amounts[i],available);
            std::array<float,4> tint=available?std::array<float,4>{1,1,1,1}:std::array<float,4>{0.4f,0.4f,0.4f,0.55f};
            craft_call(ui.currency_icons[i].get(),"SetColorAndOpacity",tint.data(),16);ui.currency_enabled[i]=available;}
        const uint64_t id=row&&!row->native_id.empty()?
            (row->native_id.rfind("I_SHARD_",0)==0?native_literal_name(row->native_id):native_name(row->native_id)):0;
        feedback_visibility(ui.currency_icons[i].get(),id?3:1);
        const std::array<float,2> icon_layout{layout.row_y[i]+2,layout.icon_size};
        if(id&&(id!=ui.currency_icon_ids[i]||icon_layout!=ui.currency_icon_layout[i])) {
            craft_texture_fit(ui.currency_icons[i].get(),craft_item_icon(ui,id,-1,0),1218,icon_layout[0],layout.icon_size,layout.icon_size);
            ui.currency_icon_ids[i]=id;ui.currency_icon_layout[i]=icon_layout;
        }
        const float text_y=layout.row_y[i]+(layout.row_height==60?8:12);
        feedback_rect(ui.choices[i].widget.get(),id?1294.0f:1224.0f,text_y,id?492.0f:622.0f,50);
        feedback_rect(ui.amounts[i].widget.get(),1790,text_y,58,50);
    }
    craft_text_set(ui.wallet,m.stage()==itemization::CraftScreenStage::Equipment || m.stage()==itemization::CraftScreenStage::Currency?"":m.wallet_summary());
    craft_details_render(ui);
    craft_text_set(ui.description,craft_page_text(!salvage && !ui.can_craft && m.stage()==itemization::CraftScreenStage::Equipment?
        "Choose generated equipment to craft. Your currency balances remain visible.":m.explanation(),44,ui.detail_page,salvage?7:5));
    feedback_visibility(ui.choice_selection.get(),m.stage()!=itemization::CraftScreenStage::Equipment&&!rows.empty()?3:1);
    feedback_rect(ui.choice_selection.get(),1210,layout.selected_y,638,layout.row_height-2);
    if(const auto arrow=ui.selection_arrow.get()) {
        const bool equipment=m.stage()==itemization::CraftScreenStage::Equipment;
        feedback_visibility(arrow,(equipment?!ui.inventory.selected().id.empty():!rows.empty())?3:1);
        feedback_rect(arrow,equipment?34+static_cast<float>(ui.inventory.column())*120:1170,
            equipment?250+static_cast<float>(ui.inventory.visible_row())*68:layout.selected_y+14,40,40);
    }
    // SetLayout updates native slot coordinates, but cached drawing may remain
    // unchanged when the next item's icon, name and stats are identical. Focus
    // is positional: invalidate its owning canvas independently of text caches.
    const std::array<size_t,8> focus{static_cast<size_t>(m.stage()),ui.inventory.column(),
        ui.inventory.visible_row(),ui.inventory.bag_first(),m.choice_cursor(),static_cast<size_t>(m.currency_tab()),
        static_cast<size_t>(layout.selected_y),static_cast<size_t>(layout.row_height)};
    if(!ui.focus_painted||ui.painted_focus!=focus) {
        craft_call(ui.canvas.get(),"InvalidateLayoutAndVolatility",nullptr,0);
        ui.painted_focus=focus;ui.focus_painted=true;
    }
}
bool craft_screen_is_open() {return craft_ui && craft_ui->host.get();}
void craft_close() {
    if(!craft_ui) return;auto ui=std::move(craft_ui);const auto manager=ui->manager.get(),host=ui->host.get();
    equipment_lock_hide();
    const auto origin=ui->origin.get();
    const bool exit_main=origin&&read<uint32_t>(read<uintptr_t>(origin+0x10)+0x18)==craft_main_class_name;
    const bool before=craft_constructing;craft_constructing=true;
    craft_return_origin=ui->origin;craft_return_back_pending=true;
    craft_return_input_until=GetTickCount64()+50; // Only absorb the closing frame's input, never a later Back press.
    // Every cleanup step runs independently: a page-removal error must not
    // leave the camera locked or the originating menu hidden.
    const auto cleanup=[&](const char* step,auto action){try{action();}catch(const std::exception& e){log({{"event","crafting_close_error"},{"step",step},{"reason",e.what()}});}};
    cleanup("remove_page",[&]{if(manager && host) {struct {uintptr_t widget;uint8_t children,focus,accept;} p{host,1,1,1};craft_call(manager,"RemoveUMGWidget",&p,11);}});
    cleanup("release_camera",[&]{if(manager && ui->camera_locked){std::array<uint8_t,2> p{0,0};craft_call(manager,"SetUIDisableCamera",p.data(),2);ui->camera_locked=false;}});
    for(const auto& hidden:ui->hidden_origins)cleanup("restore_visibility",[&]{if(const auto widget=hidden.first.get())feedback_visibility(widget,hidden.second);});
    cleanup("restore_focus",[&]{if(!exit_main&&manager) if(const auto origin=ui->origin.get()) {
        uint8_t enabled=1;craft_call(origin,"SetWidgetActive",&enabled,1);
        craft_call(origin,"EnableKeyEvent",&enabled,1);
        craft_call(origin,"ForceLayoutPrepass",nullptr,0);
        craft_call(manager,"FocusUMGWidgetByWidgetRef",const_cast<uintptr_t*>(&origin),8);
        if(const auto selected=ui->origin_selected.get()) craft_call(origin,"SelectItemFromRef",const_cast<uintptr_t*>(&selected),8);
    }});
    // An exit from main-menu Crafting returns all the way to the world. Merely
    // hiding/restoring its command widget leaves the manager in menu mode (gold
    // HUD visible and Y ignored until another Back). Use its complete teardown.
    cleanup("close_main_menu",[&]{if(exit_main&&manager){uint8_t destructed=0;craft_call(manager,"CloseMenu",&destructed,1);}});
    // The message selection window belongs to the dialogue controller. Its
    // native page transition can collapse it while our inventory owns focus.
    // Reparse the pending home page rather than restoring that stale window.
    cleanup("restore_forge_home",[&]{if(const auto controller=ui->forge_controller.get()) {
        const auto p=forge_prompts.find(controller);
        if(p==forge_prompts.end() || !p->second->home || p->second->finished)
            throw std::runtime_error("Pending forge home is unavailable");
        forge_prompt_next_page(reinterpret_cast<void*>(controller),p->second);
        log({{"event","campsite_forge_home_resumed"}});
    }});
    craft_constructing=before;
    Json timing=Json::object();for(const auto& sample:craft_menu_samples) timing[sample.first]={{"calls",sample.second.calls},{"worst_ms",sample.second.worst_ms},{"mean_ms",sample.second.total_ms/sample.second.calls}};
    uintptr_t focused=0;cleanup("verify_focus",[&]{if(manager)craft_call(manager,"GetUMGWidgetInFocus",&focused,8);});
    log({{"event","standalone_crafting_closed"},{"return_mode",exit_main?"world":"parent"},{"origin_focus_restored",!exit_main&&focused==ui->origin.get()},{"menu_timings",timing}});
}
void craft_screen_open(bool salvage,uintptr_t caller,uintptr_t forge_controller) {
    if(craft_screen_is_open()) return;
    equipment_lock_hide();
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
    craft_menu_samples.clear();CraftMenuTiming timing("open");
    const auto manager=craft_ui_manager(),root=read<uintptr_t>(manager+0x268);
    const auto cls=craft_template_class(manager);
    const auto gear=craft_inventory(find_manager(),salvage);
    auto ui=std::make_unique<CraftUI>();ui->manager=FeedbackWeak::from(manager);ui->root=FeedbackWeak::from(root);
    if(forge_controller)ui->forge_controller=FeedbackWeak::from(forge_controller);
    uintptr_t origin=0;craft_call(manager,"GetUMGWidgetInFocus",&origin,8);if(!origin)origin=caller;
    if(origin) {ui->origin=FeedbackWeak::from(origin);if(feedback_is_a(origin,"JackUMGWindowBase")) if(const auto selected=read<uintptr_t>(origin+0x3A0)) ui->origin_selected=FeedbackWeak::from(selected);}
    // RootPanel (2) attaches a visual root only. Menu (3) registers a native
    // page, binds menu keys, and participates in focus/Back restoration.
    struct {uintptr_t cls;uint8_t target,new_page,focus;uint8_t padding[5];uintptr_t result;} p{cls,3,1,1,{},0};
    craft_constructing=true;try {craft_call(manager,"AddUMGConnectWidgetWithClass",&p,24);} catch(...) {craft_constructing=false;throw;}craft_constructing=false;
    if(!p.result) throw std::runtime_error("Independent crafting page did not open");
    const auto custom_weak=FeedbackWeak::from(p.result);craft_custom_hosts.insert({custom_weak.index,custom_weak.serial});
    ui->host=FeedbackWeak::from(p.result);ui->host_address=p.result;craft_ui=std::move(ui);
    try {
        const auto host=p.result,tree=read<uintptr_t>(host+0x168),canvas=read<uintptr_t>(host+0x418);
        feedback_require_property(read<uintptr_t>(host+0x10),"RootPanel",0x418);
        // This is a child page, not a second main menu. Its inherited MainClose
        // preset otherwise changes the root menu's state when this page exits.
        feedback_require_property(read<uintptr_t>(host+0x10),"bIsTopMenu",0x20C);
        feedback_require_property(read<uintptr_t>(host+0x10),"ClosePresetAnimOption",0x208);
        *reinterpret_cast<uint8_t*>(host+0x20C)=0;
        *reinterpret_cast<uint8_t*>(host+0x208)=11; // EJackUMGPresetAnim::None
        uint8_t enabled=1;
        craft_call(host,"SetIsFocusableWindow",&enabled,1);craft_call(host,"SetWidgetActive",&enabled,1);
        craft_call(host,"EnableKeyEvent",&enabled,1);
        *reinterpret_cast<uint8_t*>(host+0x20F)=1;*reinterpret_cast<uint8_t*>(host+0x210)=1;
        int budget=1024;const auto font=feedback_find_text(host,budget);if(!font || !canvas) throw std::runtime_error("Crafting page font/root unavailable");
        // Hide the input shell's original contents, retaining its own native
        // focus and key dispatch. The custom canvas is the sole visible UI.
        const auto children=read<Array>(canvas+0xC8);
        for(int i=0;i<children.count;++i) {const auto slot=read<uintptr_t>(children.data+uintptr_t(i)*8);if(slot) feedback_visibility(read<uintptr_t>(slot+0x40),1);}
        // Keep native key delivery enabled; the event hook consumes this
        // page's Enter/Cancel and blocks the shell's command callbacks.
        feedback_rect(host,0,0,1920,1080);
        feedback_rect(canvas,0,0,1920,1080);
        // The command shell normally sizes/positions itself for a small menu.
        // Give the independent page a fixed full-screen slot and neutral
        // native transform so its bottom controls remain within the page.
        uint8_t auto_size=0;craft_call(read<uintptr_t>(host+0x38),"SetAutoSize",&auto_size,1);
        std::array<float,2> position{0,0},scale{1,1};
        craft_call(host,"SetWidgetPosition",position.data(),8);craft_call(host,"SetWidgetScale",scale.data(),8);
        const auto page=feedback_widget(tree,feedback_canvas_class,canvas,"ARPG_StandaloneCraftingPage");feedback_rect(page,0,0,1920,1080);
        auto& u=*craft_ui;u.canvas=FeedbackWeak::from(page);
        craft_prepare_call(page,"InvalidateLayoutAndVolatility",0);
        uintptr_t members=0,items=0;craft_call(manager,"GetAccessorMembers",&members,8);craft_call(manager,"GetAccessorItem",&items,8);
        if(!members||!items)throw std::runtime_error("Current party/inventory accessors unavailable");
        u.members_accessor=FeedbackWeak::from(members);u.item_accessor=FeedbackWeak::from(items);
        u.model=std::make_unique<itemization::CraftingScreen>(*item_service,*veyl_policy,*camp_policy,
            salvage?itemization::CraftScreenKind::Salvage:itemization::CraftScreenKind::Crafting,gear,veyl_wallet(find_manager()),shard_wallet(find_manager()));
        craft_inventory_refresh(u);craft_grid_create(u,tree,page,font);
        auto text=[&](const std::string& n,float x,float y,float w,float h,int size){return craft_text_create(tree,page,font,n,x,y,w,h,size);};
        u.title=text("ARPG_CraftingTitle",280,67,640,56,36);uint8_t centre=1;craft_call(u.title.widget.get(),"SetJustification",&centre,1);
        u.gear_title=text("ARPG_CraftingGearTitle",64,160,124,42,24);
        u.selected_name=text("ARPG_CraftingSelectedName",136,669,784,42,28);
        u.lock_hint=text("ARPG_CraftingLockHint",936,665,212,50,18);craft_pale_text(u.lock_hint);
        u.currency_title=text("ARPG_CraftingCurrencyTitle",1220,175,622,42,26);
        const std::string tab_art="/Game/UIs_modern/Texture/T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002";
        u.veyl_tab_frame=FeedbackWeak::from(craft_art(tree,page,"ARPG_VeylTabFrame",tab_art,1210,167,202,50,false,false,{1,0.76f,0.22f,0.35f}));
        u.shard_tab_frame=FeedbackWeak::from(craft_art(tree,page,"ARPG_ShardTabFrame",tab_art,1424,167,202,50,false,false,{1,0.76f,0.22f,0.35f}));
        u.suffix_tab_frame=FeedbackWeak::from(craft_art(tree,page,"ARPG_SuffixTabFrame",tab_art,1638,167,202,50,false,false,{1,0.76f,0.22f,0.35f}));
        u.veyl_tab=text("ARPG_VeylTab",1210,175,202,42,24);craft_text_set(u.veyl_tab,"Veyls");
        u.shard_tab=text("ARPG_ShardTab",1424,175,202,42,24);craft_text_set(u.shard_tab,"Prefix shards");
        u.suffix_tab=text("ARPG_SuffixTab",1638,175,202,42,24);craft_text_set(u.suffix_tab,"Suffix shards");
        craft_call(u.veyl_tab.widget.get(),"SetJustification",&centre,1);
        craft_call(u.shard_tab.widget.get(),"SetJustification",&centre,1);
        craft_call(u.suffix_tab.widget.get(),"SetJustification",&centre,1);
        for(size_t s=0;s<u.currency_sections.size();++s) {
            u.currency_separators[s]=FeedbackWeak::from(craft_art(tree,page,"ARPG_ShardSeparator"+std::to_string(s),
                "/Game/UIs_common/Texture/T_Hud_Parts_004.T_Hud_Parts_004",1218,236,626,2,false,false,{0.44f,0.24f,0.08f,0.55f}));
            u.currency_sections[s]=text("ARPG_ShardSection"+std::to_string(s),1224,236,622,28,18);
            craft_pale_text(u.currency_sections[s]);
        }
        // Reuse the native short confirmation cue, owned by the manager's
        // sound set. Resolve once per page, not by scanning assets per craft.
        uintptr_t sounds=0;craft_call(manager,"GetSoundSet",&sounds,8);
        if(sounds&&feedback_is_a(sounds,"JackUMGSoundSet")) {
            feedback_require_property(read<uintptr_t>(sounds+0x10),"BasicEnter",0x58);
            u.craft_success_sound=FeedbackWeak::from(read<uintptr_t>(sounds+0x58));
        }
        craft_prepare_call(host,"PlayUISound",12);
        for(size_t i=0;i<itemization::craft_currency_rows;++i) {
            const auto image=feedback_image(tree,page,"ARPG_CurrencyIcon"+std::to_string(i),{1,1,1,1});u.currency_icons[i]=FeedbackWeak::from(image);
            feedback_rect(image,1218,238+static_cast<float>(i)*68,62,62);
            u.choices[i]=text("ARPG_CraftingChoice"+std::to_string(i),1294,248+static_cast<float>(i)*68,492,50,22);
            u.amounts[i]=text("ARPG_CraftingAmount"+std::to_string(i),1790,248+static_cast<float>(i)*68,58,50,22);
            uint8_t right=2;craft_call(u.amounts[i].widget.get(),"SetJustification",&right,1);
        }
        u.wallet=text("ARPG_CraftingWallet",1224,724,622,54,13);
        u.rarity_badge=text("ARPG_RarityBadge",80,724,244,32,21);craft_pale_text(u.rarity_badge);
        u.class_badge=text("ARPG_ClassBadge",372,724,244,32,21);craft_pale_text(u.class_badge);
        u.quality_badge=text("ARPG_QualityBadge",664,724,244,32,21);craft_pale_text(u.quality_badge);
        u.details=text("ARPG_CraftingItemDetails",68,788,1064,242,21);craft_pale_text(u.details);
        for(size_t i=0;i<u.detail_rows.size();++i) {
            const float y=788+static_cast<float>(i)*28.5f;auto& row=u.detail_rows[i];
            row.highlight=FeedbackWeak::from(craft_art(tree,page,"ARPG_AffixChangeHighlight"+std::to_string(i),
                tab_art,62,y-1,1076,29,false,false,{1,0.82f,0.05f,0.28f}));
            row.text=text("ARPG_AffixChangeText"+std::to_string(i),68,y,1064,29,21);craft_pale_text(row.text);
        }
        u.detail_footer=text("ARPG_AffixChangePages",68,1017,1064,23,16);craft_pale_text(u.detail_footer);
        u.description=text("ARPG_CraftingDescription",1220,salvage?718:846,622,salvage?252:174,21);craft_pale_text(u.description);
        // A camera lock is reference-counted by the native manager. Take one
        // lease and release precisely that lease on close, including errors.
        std::array<uint8_t,2> camera{1,0};craft_call(manager,"SetUIDisableCamera",camera.data(),2);u.camera_locked=true;
        if(const auto origin_widget=u.origin.get()) {
            const auto cursor=feedback_is_a(origin_widget,"JackUMGWindowBase")?read<uintptr_t>(origin_widget+0x3B8):0;
            if(cursor){u.hidden_origins.emplace_back(FeedbackWeak::from(cursor),read<uint8_t>(cursor+0x86));feedback_visibility(cursor,1);}
            u.hidden_origins.emplace_back(FeedbackWeak::from(origin_widget),read<uint8_t>(origin_widget+0x86));feedback_visibility(origin_widget,2); // Hidden preserves layout for cursor restoration.
        }
        if(const auto controller=u.forge_controller.get()) {
            uintptr_t dialogue=0;craft_call(controller,"GetSerifuWindow",&dialogue,8);
            if(!dialogue||!feedback_is_a(dialogue,"JackUMGSerifuWindow"))throw std::runtime_error("Forge dialogue window unavailable");
            const auto dialogue_tree=read<uintptr_t>(dialogue+0x168);
            const auto contents=dialogue_tree?read<uintptr_t>(dialogue_tree+0x38):0;
            if(!contents)throw std::runtime_error("Forge dialogue contents unavailable");
            // The controller updates its outer window while waiting. Hide its
            // content root, so native window updates cannot draw over inventory.
            u.hidden_origins.emplace_back(FeedbackWeak::from(contents),read<uint8_t>(contents+0x86));
            feedback_visibility(contents,1);
            log({{"event","campsite_forge_dialogue_suspended"}});
        }
        // Discard the command shell's hidden list; selection is wholly owned
        // by this inventory and never points back at vanilla commands.
        craft_call(host,"EmptyStaticItemList",nullptr,0);
        uintptr_t empty=0;craft_call(host,"SelectItemFromRef",&empty,8);
        craft_render();
        craft_call(manager,"FocusUMGWidgetByWidgetRef",const_cast<uintptr_t*>(&host),8);
        uintptr_t focused=0;craft_call(manager,"GetUMGWidgetInFocus",&focused,8);
        if(focused!=host)throw std::runtime_error("Crafting page did not receive native menu focus");
        // The shell's own arrow points at its hidden command entries. Our
        // visible selection belongs to the crafting model, not that list.
        if(const auto cursor=read<uintptr_t>(host+0x3B8))feedback_visibility(cursor,1);
        log({{"event","standalone_crafting_opened"},{"screen",salvage?"salvage":"veyl_shards"},{"gear",gear.size()},{"native_background",true},{"native_focus_verified",true},{"forge_session",false}});
    } catch(...) {craft_close();throw;}
}
void craft_key(uint8_t key,bool pressed,bool repeated=false) {
    if(!pressed || !craft_screen_is_open()) return;
    CraftMenuTiming timing("navigation");
    std::lock_guard<std::recursive_mutex> lock(runtime_mutex);auto& u=*craft_ui;
    // Enter/Cancel are already resolved by Jack, including swapped buttons.
    auto& m=*u.model;const auto old_source=m.source();bool crafted_currency=false;
    const bool equipment=m.stage()==itemization::CraftScreenStage::Equipment;
    if(key==23) {if(equipment)u.inventory.vertical(-1);else m.move(-1);}
    else if(key==24) {if(equipment)u.inventory.vertical(1);else m.move(1);}
    else if(key==8 || key==11) {if(!equipment)m.switch_tab();}
    else if(key==9 || key==12) {if(equipment)u.inventory.vertical(key==9?-1:1,true);else m.move(key==9?-1:1,true);}
    else if(key==25) {if(equipment)u.inventory.horizontal(-1);else m.left();}
    else if(key==28) m.back();
    else if(key==26) {if(equipment){const bool edge=u.inventory.horizontal(1);if(edge&&m.kind()==itemization::CraftScreenKind::Salvage)m.right();}else if(m.stage()==itemization::CraftScreenStage::Currency)m.switch_tab();else m.right();}
    else if(key==27) ++u.detail_page;
    else if(key==29) {
        if(equipment&&!u.can_craft)return;
        const auto request=m.enter();
        if(!request.empty()) {
            std::string message,result_id;
            Json before,after;
            try {
                if(request.veyl||!request.shard.empty())before=item_service->lookup(request.source);
                if(request.veyl) {const auto changed=standalone_veyl_commit(native_name(request.source),*request.veyl,request.cost);after=changed;result_id=changed.at("engine_id");message=veyl_policy->definition(request.veyl->currency).at("name").get<std::string>()+" applied.\n"+itemization::instance_name(changed);}
                else if(!request.shard.empty()) {const auto changed=standalone_shard_commit(native_name(request.source),request.shard);after=changed;result_id=changed.at("engine_id");message=camp_policy->definition(request.shard).at("name").get<std::string>()+" applied.\n"+itemization::instance_name(changed);}
                else {const auto outcome=salvage_commit(request.salvage);const auto& drops=outcome.at("shards");message="Salvaged "+outcome.at("salvaged").dump()+" items.";if(outcome.at("skipped_locked").get<int>())message+="\nSkipped "+outcome.at("skipped_locked").dump()+" locked items.";if(drops.empty()) message+="\nNo shards recovered.";for(auto d=drops.begin();d!=drops.end();++d) message+="\n"+camp_policy->definition(d.key()).at("name").get<std::string>()+" x"+d.value().dump();}
            } catch(const std::exception& e) {message=std::string(request.salvage.empty()?"Craft was refused.\n":"Salvage was refused.\n")+e.what();log({{"event","standalone_crafting_refused"},{"reason",e.what()}});}
            const bool completed_currency=(request.veyl.has_value()||!request.shard.empty())&&!result_id.empty();
            crafted_currency=completed_currency;
            m.refresh(craft_inventory(find_manager(),m.kind()==itemization::CraftScreenKind::Salvage),veyl_wallet(find_manager()),shard_wallet(find_manager()),message,result_id,completed_currency);
            craft_inventory_refresh(u,result_id);u.detail_page=0;
            if(completed_currency)try {
                auto changes=itemization::craft_changes(*item_service,before,after);
                u.change_notes[changes.instance_id]=std::move(changes);u.detail_lines_key.clear();craft_details_prepare(u);
                u.detail_page=itemization::craft_first_change_page(u.detail_lines);
                u.pulse_item=result_id;u.pulse_started=u.change_notes.at(after.at("instance_id").get<std::string>()).empty()?0:GetTickCount64();u.next_pulse_frame=0;
            }catch(const std::exception& e){log({{"event","crafting_change_feedback_error"},{"reason",e.what()}});}
            if(completed_currency)try {
                if(const auto sound=u.craft_success_sound.get()) {
                    struct {uintptr_t sound;int32_t monophonic_id;} p{sound,0};
                    craft_call(u.host.get(),"PlayUISound",&p,12);
                }
            }catch(const std::exception& e){log({{"event","crafting_success_sound_error"},{"reason",e.what()}});}
        }
    } else return;
    if(!u.input_verified) {u.input_verified=true;log({{"event","standalone_crafting_input_received"},{"key",key}});}
    if(m.stage()==itemization::CraftScreenStage::Closed) {craft_close();return;}
    if(m.stage()==itemization::CraftScreenStage::Equipment)u.can_craft=u.inventory.selected().craftable&&m.select_source(u.inventory.selected().id);
    if(!crafted_currency&&(m.source()!=old_source || u.presentation_id!=u.inventory.selected().id)) u.detail_page=0;craft_render();
}
bool craft_window_input(void* window,const void* status) {
    if(craft_ui && reinterpret_cast<uintptr_t>(window)==craft_ui->host_address && status) {
        // UJackUMGWindowBase's native key record contains the logical key at
        // +0 and Pressed/Repeated at +0x0F/+0x11. Intercept before MoveCursor
        // and selected-command callbacks, not after the Blueprint Enter event.
        const auto record=reinterpret_cast<uintptr_t>(status);const int key=read<int32_t>(record);
        const bool pressed=read<uint8_t>(record+0x0F)!=0;
        const bool repeat=read<uint8_t>(record+0x11)!=0;
        try {if(itemization::crafting_input_action(key,pressed,repeat))craft_key(static_cast<uint8_t>(key),true,repeat);}
        catch(const std::exception& e){log({{"event","crafting_inventory_input_refused"},{"reason",e.what()}});if(craft_ui)craft_close();}
        return true;
    }
    const auto native_window=reinterpret_cast<uintptr_t>(window);
    // The parent remains registered behind our page. It must not act on the
    // same key dispatch and open a vanilla page or close its menu underneath us.
    if(craft_ui && native_window==craft_ui->origin.get())return true;
    if(craft_return_back_pending && GetTickCount64()>craft_return_input_until)craft_return_back_pending=false;
    if(native_window && craft_return_back_pending && craft_return_origin.get()==native_window && status) {
        const auto record=reinterpret_cast<uintptr_t>(status);const int key=read<int32_t>(record);
        if(key==28 || key==7 || key==21) {
            if(read<uint8_t>(record+0x0F)||read<uint8_t>(record+0x11))return true;
            craft_return_back_pending=false;
        } else if(read<uint8_t>(record+0x0F))craft_return_back_pending=false;
    }
    if(native_window && read<uint32_t>(read<uintptr_t>(native_window+0x10)+0x18)==craft_main_class_name && craft_custom_host(native_window))return true;
    if(!craft_ui && native_window && read<uint32_t>(read<uintptr_t>(native_window+0x10)+0x18)==craft_main_class_name) {
        try{craft_add_main_entry(native_window);}catch(const std::exception& e){log({{"event","crafting_main_input_setup_refused"},{"reason",e.what()}});}
    }
    try{if(equipment_lock_needs_refresh(native_window))equipment_lock_add_entry(native_window);}catch(const std::exception& e){log({{"event","equipment_lock_menu_setup_refused"},{"reason",e.what()}});}
    return original_craft_window_input(window,status);
}
void craft_layout_main_entry(uintptr_t owner,CraftMainEntry& entry) {
    const auto row=entry.row.get(),label=entry.label.get();if(!row || !label)return;
    const auto canvas=read<uintptr_t>(owner+0x410);
    const auto slots=read<Array>(canvas+0xC8);
    if(slots.count<1 || slots.count>32 || slots.capacity<slots.count)throw std::runtime_error("Main menu row layout unavailable");
    float left=10000,top=10000,bottom=0,height=0,width=0,font_y=-1;uintptr_t font_source=0,above=0;
    // The campsite variant inserts another native row and shifts every command.
    // Read only this small canvas; never scan the world's widget collection.
    for(int i=0;i<slots.count;++i) {
        const auto slot=read<uintptr_t>(slots.data+uintptr_t(i)*8);if(!slot)continue;
        const auto child=read<uintptr_t>(slot+0x40);if(!child || child==row || read<uint8_t>(child+0x86)==1 || read<uint8_t>(child+0x86)==2)continue;
        if(read<uintptr_t>(child+0x10)!=read<uintptr_t>(row+0x10))continue;
        const float x=read<float>(slot+0x48),y=read<float>(slot+0x4C),w=read<float>(slot+0x50),h=read<float>(slot+0x54);
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||x<0||y<0||w<=0||h<=0||w>2000||h>200)continue;
        if(x<left || (x==left && y>=font_y)) {left=x;width=w;font_y=y;above=child;int budget=64;font_source=feedback_find_text(child,budget);}
        top=std::min(top,y);bottom=std::max(bottom,y+h);height=std::max(height,h);
    }
    if(!font_source || height<=0)return; // Native Construct has not laid out its rows yet.
    const auto slot=read<uintptr_t>(row+0x38);
    const bool moved=read<float>(slot+0x48)!=left || read<float>(slot+0x4C)!=bottom || read<float>(slot+0x50)!=width || read<float>(slot+0x54)!=height;
    if(moved) {
        feedback_rect(row,left,bottom,width,height);
        // CanvasSlot.SetLayout moves the text but leaves JackUMG's Position
        // cache unchanged. Native UpdateCursorPosition reads that cache.
        // Use the native setter too, so the camp header cannot leave the
        // arrow at the earlier (ordinary-menu) row position.
        struct {float x,y;} position{left,bottom};
        craft_call(row,"SetWidgetPosition",&position,8);
        entry.layout_pending=true;
    }
    // Slate visibility alone is insufficient: native MoveCursor checks
    // JackUMGWidgetBase.bIsVisible (+0x1F8) before considering a candidate.
    if(!read<uint8_t>(row+0x1F8)) {uint8_t visible=1;craft_call(row,"SetWidgetVisible",&visible,1);}
    // DataNumber belongs to SetItemList. Do not change it after registration.
    // The native campsite links can bypass geometric cursor searches. Extend
    // only the edge to/from our row; preserve every other native transition.
    auto link=[&](uintptr_t from,int direction,uintptr_t to) {
        const auto moves=read<Array>(owner+0x3F0);
        if(moves.count<0 || moves.count>128 || moves.capacity<moves.count)throw std::runtime_error("Main menu cursor links unavailable");
        for(int i=0;i<moves.count;++i) {
            const auto address=moves.data+uintptr_t(i)*24;
            if(read<uintptr_t>(address)==from && read<int32_t>(address+8)==direction) {
                if(read<uintptr_t>(address+16)!=to)*reinterpret_cast<uintptr_t*>(address+16)=to;
                return;
            }
        }
        struct {uintptr_t from;int direction,padding;uintptr_t to;} setting{from,direction,0,to};
        craft_call(owner,"AddCursorMoveSetting",&setting,24);
    };
    link(above,3,row);link(row,1,above); // Native directions: Right, Up, Left, Down.
    const auto root=read<uintptr_t>(owner+0x418),root_slot=read<uintptr_t>(root+0x38);
    if(!root_slot)throw std::runtime_error("Main menu frame slot unavailable");
    // A campsite header occupies a whole row above the commands; it is not
    // extra border padding. Both layouts retain the native 24-unit inset.
    const float padding=std::fmod(top,height);
    const float root_width=read<float>(root_slot+0x50),required_height=bottom+height+padding;
    if(read<float>(root_slot+0x54)!=required_height) {feedback_rect(root,0,0,root_width,required_height);entry.layout_pending=true;}
    std::array<uint8_t,0x58> font{};std::memcpy(font.data(),reinterpret_cast<void*>(font_source+0x158),font.size());
    if(!entry.styled || font!=entry.font) {
        feedback_event(label,"TextBlock.SetFont",font.data());entry.font=font;entry.styled=true;
    }
}
void craft_add_main_entry(uintptr_t owner) {
    if(craft_constructing || !camp_policy || !runtime_enabled.load()) return;
    if(craft_custom_host(owner))return;
    const auto weak=FeedbackWeak::from(owner);const auto key=std::make_pair(weak.index,weak.serial);
    if(const auto found=craft_main_entries.find(key);found!=craft_main_entries.end()) {
        if(found->second.row.get()) {
            craft_layout_main_entry(owner,found->second);
            const auto items=read<Array>(owner+0x380);bool registered=false;
            if(items.count<0 || items.count>32 || items.capacity<items.count)throw std::runtime_error("Main menu selectable list unavailable");
            for(int i=0;i<items.count;++i)if(read<uintptr_t>(items.data+uintptr_t(i)*8)==found->second.row.get())registered=true;
            if(!registered) {
                struct {uintptr_t canvas,item;} list{read<uintptr_t>(owner+0x410),read<uintptr_t>(owner+0x3A0)};
                craft_call(owner,"SetItemList",&list,16);found->second.layout_pending=true;
            }
            // Reposition once after the added row has passed a layout frame.
            // Cursor geometry queried during Construct can still be zero.
            if(found->second.layout_pending && read<uint8_t>(owner+0x86)==0) {
                const auto selected=read<uintptr_t>(owner+0x3A0);
                if(selected)craft_call(owner,"SelectItemFromRef",const_cast<uintptr_t*>(&selected),8);
                found->second.layout_pending=false;
            }return;
        }craft_main_entries.erase(found);
    }
    const auto started=GetTickCount64();
    const auto manager=craft_ui_manager(owner);craft_bind_style(owner,manager);
    const auto existing=read<uintptr_t>(owner+0x420);if(!existing) throw std::runtime_error("Main menu item template missing");
    feedback_require_property(read<uintptr_t>(owner+0x10),"ItemList",0x410);
    const auto row=craft_create_widget(manager,read<uintptr_t>(existing+0x10));
    feedback_require_property(read<uintptr_t>(row+0x10),"bIsVisible",0x1F8);
    struct {uintptr_t content,result;} attach{row,0};feedback_event(read<uintptr_t>(owner+0x410),"CanvasPanel.AddChildToCanvas",&attach);
    feedback_rect(row,44,162,240,46);
    uint8_t auto_size=0;craft_call(read<uintptr_t>(row+0x38),"SetAutoSize",&auto_size,1);
    *reinterpret_cast<uint8_t*>(row+0x370)=1;*reinterpret_cast<uint64_t*>(row+0x380)=native_name("ARPG_CRAFTING_ENTRY");
    int budget=512;const auto label=feedback_find_text(row,budget);if(!label) throw std::runtime_error("Main menu crafting caption unavailable");feedback_set_text(label,"Crafting");
    struct {uintptr_t canvas,item;} list{read<uintptr_t>(owner+0x410),read<uintptr_t>(owner+0x3A0)};craft_call(owner,"SetItemList",&list,16);
    CraftMainEntry entry{weak,FeedbackWeak::from(row),FeedbackWeak::from(label)};
    craft_layout_main_entry(owner,entry);
    craft_call(owner,"ForceLayoutPrepass",nullptr,0);
    if(list.item) craft_call(owner,"SelectItemFromRef",&list.item,8);
    craft_main_entries.emplace(key,std::move(entry));
    log({{"event","crafting_main_menu_entry_added"},{"native_selectable_row",true},{"setup_ms",GetTickCount64()-started}});
}
void craft_event_hook(void* object,void* function,void* params) {
    if(craft_internal_event || !craft_ui_enabled || !runtime_enabled.load()) {original_craft_event(object,function,params);return;}
    const auto o=reinterpret_cast<uintptr_t>(object),f=reinterpret_cast<uintptr_t>(function);
    // ProcessEvent supplies valid engine objects. Keep this global dispatch
    // filter to primitive comparisons; never traverse reflected hierarchies
    // or resolve strings for unrelated widgets.
    const auto name_index=*reinterpret_cast<const uint32_t*>(f+0x18);
    const bool main=*reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uintptr_t*>(o+0x10)+0x18)==craft_main_class_name;
    const bool own=craft_ui && o==craft_ui->host_address;
    const bool lock_menu=*reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uintptr_t*>(o+0x10)+0x18)==equipment_lock_command_name;
    if(!own && !main){
        if(lock_menu&&name_index==craft_control_name)try{if(equipment_lock_menu_event(o,f,params))return;}
        catch(const std::exception& e){log({{"event","equipment_lock_menu_action_refused"},{"reason",e.what()}});return;}
        if(lock_menu&&(name_index==equipment_lock_create_name||name_index==equipment_lock_setup_name))try{equipment_lock_restore(o,true);}catch(const std::exception& e){log({{"event","equipment_lock_menu_restore_refused"},{"reason",e.what()}});}
        inventory_rarity_event(o,f,params,name_index,true);
        original_craft_event(object,function,params);
        inventory_rarity_event(o,f,params,name_index,false);
        if(lock_menu)try{
            const bool rebuilt=name_index==equipment_lock_create_name;
            if(rebuilt)equipment_lock_add_entry(o);
            else if(name_index==craft_update_name&&equipment_lock_needs_refresh(o))equipment_lock_add_entry(o);
        }catch(const std::exception& e){static std::set<std::string> seen;if(seen.insert(e.what()).second)log({{"event","equipment_lock_menu_setup_refused"},{"reason",e.what()}});}
        return;
    }
    try {
        if(main && craft_constructing && name_index==craft_construct_name) {
            feedback_require_property(read<uintptr_t>(o+0x10),"bIsTopMenu",0x20C);
            feedback_require_property(read<uintptr_t>(o+0x10),"ClosePresetAnimOption",0x208);
            *reinterpret_cast<uint8_t*>(o+0x20C)=0;*reinterpret_cast<uint8_t*>(o+0x208)=11;
            const auto weak=FeedbackWeak::from(o);craft_custom_hosts.insert({weak.index,weak.serial});
        }
        const bool custom=main && craft_custom_host(o);
        // Use the page's existing UI update callback only. Keep the retired
        // shell's own command updates blocked, and stop ticking after the cue.
        if(own&&name_index==craft_update_name){craft_change_tick();return;}
        if(custom || (main && craft_constructing)) {
            if(name_index==craft_control_name || name_index==craft_key_name || name_index==craft_update_name)return;
            if(std::find(craft_blocked_names.begin(),craft_blocked_names.end(),name_index)!=craft_blocked_names.end())return;
        }
        if(own && craft_ui->host.get()==o) {
            if(name_index==craft_key_name && params) return; // Native input hook owns the complete key record.
            if(name_index==craft_control_name || name_index==craft_update_name) return;
            if(std::find(craft_blocked_names.begin(),craft_blocked_names.end(),name_index)!=craft_blocked_names.end()) return;
        }
        if(name_index==craft_control_name && params && !craft_constructing) {
            const auto item=read<uintptr_t>(reinterpret_cast<uintptr_t>(params)+8);
            const auto weak=FeedbackWeak::from(o);const auto e=craft_main_entries.find({weak.index,weak.serial});
            if(e!=craft_main_entries.end() && e->second.row.get()==item && read<uint64_t>(reinterpret_cast<uintptr_t>(params))==read<uint64_t>(base+0x5C1E3B0)) {craft_screen_open(false,o);return;}
        }
    } catch(const std::exception& e) {log({{"event","crafting_ui_event_refused"},{"reason",e.what()}});if(craft_ui) craft_close();return;}
    original_craft_event(object,function,params);
    if(!craft_constructing && !craft_ui && (name_index==craft_update_name || name_index==craft_construct_name) && main) {
        try {craft_add_main_entry(o);} catch(const std::exception& e) {static std::set<std::string> reported;if(reported.insert(e.what()).second) log({{"event","crafting_main_menu_not_ready"},{"reason",e.what()}});}
    }
}
void create_crafting_ui_hook() {
    if(!camp_policy) return;
    const std::vector<std::pair<uintptr_t,std::vector<uint8_t>>> guards{
        {0xEF12A0,{0x40,0x55,0x56,0x57,0x41}},
        {0x7692A0,{0x44,0x89,0x4C,0x24,0x20}},
        {0x76EBC0,{0x48,0x89,0x5C,0x24,0x20}},
        {0x7EF460,{0x48,0x83,0xEC,0x28,0xE8}},
        {0x1678580,{0x48,0x89,0x5C,0x24,0x08}},
        {0x93C400,{0x83,0xF9,0x07}},
        {0x9DDE50,{0x48,0x89,0x5C,0x24,0x10}},
        {0x16B0A00,{0x48,0x83,0xEC,0x78,0x48}},
        {0x16B0F10,{0x48,0x83,0xEC,0x38,0x0F}},
        {0x7FFA20,{0x33,0xC9,0x8B,0xC1,0x48}},
        {0x7FF7A4,{0x4D,0x8B,0x44,0x24,0x38,0x49,0x63,0x7C,0x24,0x40}}};
    for(const auto& g:guards) for(size_t i=0;i<g.second.size();++i) if(read<uint8_t>(base+g.first+i)!=g.second[i]) throw std::runtime_error("Standalone crafting native binding mismatch");
    craft_update_name=static_cast<uint32_t>(native_name("UpdateWidget"));craft_key_name=static_cast<uint32_t>(native_name("OnKeyEvent"));
    craft_control_name=static_cast<uint32_t>(native_name("OnItemListControl"));craft_construct_name=static_cast<uint32_t>(native_name("Construct"));
    craft_main_class_name=static_cast<uint32_t>(native_name("WBP_Menu_Main_Command_000_modern_C"));
    equipment_lock_command_name=static_cast<uint32_t>(native_name("WBP_Menu_Dougu_Command_000_modern_C"));
    equipment_lock_setup_name=static_cast<uint32_t>(native_name("SetupCommandList"));
    equipment_lock_create_name=static_cast<uint32_t>(native_name("CreateCommand"));
    inventory_rarity_initialize();
    size_t n=0;for(const char* name:{"WidgetBPActionDelegate","WidgetNotificationDelegate","EnableMainCommand","EnableNakamaCommand"}) craft_blocked_names[n++]=static_cast<uint32_t>(native_name(name));
    if(MH_CreateHook(reinterpret_cast<void*>(base+0xEF12A0),reinterpret_cast<void*>(&craft_event_hook),reinterpret_cast<void**>(&original_craft_event))!=MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(base+0xEF12A0))!=MH_OK) throw std::runtime_error("Standalone crafting UI hook failed");
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x9DDE50),reinterpret_cast<void*>(&craft_window_input),reinterpret_cast<void**>(&original_craft_window_input))!=MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(base+0x9DDE50))!=MH_OK)throw std::runtime_error("Crafting inventory input hook failed");
}
