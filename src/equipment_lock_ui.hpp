// Native Equipment Bag Do What menu only; salvage protection is shared.
// Protection follows logical identity, without replacing/reordering native bags.
bool equipment_lock_commit(uint64_t id) {
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex);
    const auto found=runtime_ids.find(id);if(found==runtime_ids.end())return false;
    const auto manager=find_manager();const auto stacks=veyl_stacks(manager,id);
    if(stacks.size()!=1 || stacks.front().count!=1)return false;
    const auto previous=item_service->export_history();
    const bool locked=!item_service->is_locked(found->second);
    try {item_service->set_locked(found->second,locked);persist_history();}
    catch(...) {item_service->import_history(previous);throw;}
    log({{"event","equipment_lock_changed"},{"engine_id",found->second},
        {"instance_id",item_service->lookup(found->second).at("instance_id")},{"locked",locked}});
    return true;
}
void equipment_lock_require_field(uintptr_t cls,const char* field,int offset) {
    static std::set<std::pair<uintptr_t,int>> checked;
    if(checked.count({cls,offset}))return;
    feedback_require_property(cls,field,offset);checked.emplace(cls,offset);
}

uint32_t equipment_lock_command_name=0,equipment_lock_setup_name=0,equipment_lock_create_name=0;
struct EquipmentLockEntry {
    FeedbackWeak owner,row,label,canvas,frame,background,root;
    std::string caption;
    uint64_t source=0;
    float original_background=0,original_frame=0,original_root=0,original_widget=0,original_parent=0,required=0;
    uint8_t original_visible=0,original_slate=0,original_selectable=0,original_active=0;
    bool ready_logged=false;
};
std::map<std::pair<int,int>,EquipmentLockEntry> equipment_lock_entries;
void equipment_lock_hide() {} // No independent lock screen or input layer.
bool equipment_lock_command(uintptr_t owner) {
    return owner&&equipment_lock_command_name&&read<uint32_t>(read<uintptr_t>(owner+0x10)+0x18)==equipment_lock_command_name;
}
uint64_t equipment_lock_source(uintptr_t owner) {
    if(!equipment_lock_command(owner))return 0;
    const auto cls=read<uintptr_t>(owner+0x10);
    for(const auto& f:std::map<const char*,int>{{"ItemSyousaiRef",0x4A0},{"SelectedListPanel",0x620},{"ItemListPanel",0x440}})equipment_lock_require_field(cls,f.first,f.second);
    if(read<uintptr_t>(owner+0x620)!=read<uintptr_t>(owner+0x440))return 0;
    const auto detail=read<uintptr_t>(owner+0x4A0);
    if(!detail||!feedback_is_a(detail,"JackUMGItemSyousai"))return 0;
    equipment_lock_require_field(read<uintptr_t>(detail+0x10),"BagType",0x39);
    equipment_lock_require_field(read<uintptr_t>(detail+0x10),"ItemID",0x48);
    const auto id=read<uint64_t>(detail+0x48);
    if(!itemization::equipment_lock_menu_scope("WBP_Menu_Dougu_Command_000_modern_C",read<uint8_t>(detail+0x39),runtime_ids.count(id)!=0))return 0;
    const auto stacks=veyl_stacks(find_manager(),id);
    return stacks.size()==1&&stacks.front().bag==9&&stacks.front().count==1?id:0;
}
float equipment_lock_height(uintptr_t widget) {
    const auto slot=read<uintptr_t>(widget+0x38);
    if(!slot||!feedback_is_a(slot,"CanvasPanelSlot"))throw std::runtime_error("Equipment action canvas slot unavailable");
    return read<float>(slot+0x54);
}
void equipment_lock_set_height(uintptr_t widget,float height) {
    const auto slot=read<uintptr_t>(widget+0x38);
    if(!slot||!feedback_is_a(slot,"CanvasPanelSlot"))throw std::runtime_error("Equipment action canvas slot unavailable");
    if(read<float>(slot+0x54)!=height){struct{float x,y;} size{read<float>(slot+0x50),height};craft_call(slot,"SetSize",&size,8);}
}
void equipment_lock_restore(uintptr_t owner,bool before_setup=false) {
    if(!equipment_lock_command(owner))return;
    const auto weak=FeedbackWeak::from(owner);const auto found=equipment_lock_entries.find({weak.index,weak.serial});
    if(found==equipment_lock_entries.end())return;
    const auto entry=found->second;equipment_lock_entries.erase(found);
    if(const auto row=entry.row.get()){
        uint8_t visible=entry.original_visible,active=entry.original_active;
        craft_call(row,"SetWidgetVisible",&visible,1);feedback_visibility(row,entry.original_slate);
        craft_call(row,"SetWidgetActive",&active,1);*reinterpret_cast<uint8_t*>(row+0x370)=entry.original_selectable;
    }
    if(const auto background=entry.background.get())equipment_lock_set_height(background,entry.original_background);
    if(const auto frame=entry.frame.get())equipment_lock_set_height(frame,entry.original_frame);
    // Quantity/character flows reuse this class with a wider native panel.
    // Restore parent bounds before setup, not over a newly configured flow.
    if(before_setup||read<uintptr_t>(owner+0x620)==entry.canvas.get()){
        if(const auto root=entry.root.get())equipment_lock_set_height(root,entry.original_root);
        *reinterpret_cast<float*>(owner+0x260)=entry.original_widget;
        equipment_lock_set_height(owner,entry.original_parent);
    }
}
bool equipment_lock_registered(uintptr_t owner,uintptr_t row) {
    const auto items=read<Array>(owner+0x380);
    if(items.count<0||items.count>32||items.capacity<items.count)throw std::runtime_error("Equipment selectable list unavailable");
    for(int i=0;i<items.count;++i)if(read<uintptr_t>(items.data+uintptr_t(i)*8)==row)return read<int32_t>(row+0x3BC)==i;
    return false;
}
void equipment_lock_add_entry(uintptr_t owner) {
    if(!equipment_lock_command(owner)||craft_ui||!runtime_enabled.load())return;
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex);
    const auto weak=FeedbackWeak::from(owner);const auto key=std::make_pair(weak.index,weak.serial);
    const auto source=equipment_lock_source(owner);
    if(!source){equipment_lock_restore(owner);return;}
    const auto cls=read<uintptr_t>(owner+0x10);
    for(const auto& f:std::map<const char*,int>{{"ItemListPanel",0x440},{"SmalleFramePanel",0x448},{"SmalleWindowPanel",0x450},{"SelectedItem",0x3A0},{"RootPanelRef",0x230},{"WidgetSize",0x25C}})equipment_lock_require_field(cls,f.first,f.second);
    auto found=equipment_lock_entries.find(key);
    if(found!=equipment_lock_entries.end()&&!found->second.row.get()){equipment_lock_entries.erase(found);found=equipment_lock_entries.end();}
    if(found==equipment_lock_entries.end()){
        const auto canvas=read<uintptr_t>(owner+0x440),frame=read<uintptr_t>(owner+0x448),background=read<uintptr_t>(owner+0x450),root=read<uintptr_t>(owner+0x230);
        if(!canvas||!frame||!background||!root)return;
        const auto slots=read<Array>(canvas+0xC8);
        if(slots.count<1||slots.count>32||slots.capacity<slots.count)return; // Setup is still assembling the native list.
        uintptr_t spare=0;float bottom=0,spare_y=10000;
        for(int i=0;i<slots.count;++i){
            const auto slot=read<uintptr_t>(slots.data+uintptr_t(i)*8),row=slot?read<uintptr_t>(slot+0x40):0;
            if(!row||!feedback_is_a(row,"JackUMGItemBase"))continue;
            const float y=read<float>(slot+0x4C),height=read<float>(slot+0x54);
            if(height<=0||height>100||!std::isfinite(y))continue;
            if(read<uint8_t>(row+0x1F8)&&read<uint8_t>(row+0x86)!=1&&read<uint8_t>(row+0x86)!=2)bottom=std::max(bottom,y+height);
        }
        // A native command can be hidden above later visible commands. Choose
        // the first spare after the visible list, not an earlier hidden gap.
        for(int i=0;i<slots.count;++i){
            const auto slot=read<uintptr_t>(slots.data+uintptr_t(i)*8),row=slot?read<uintptr_t>(slot+0x40):0;
            if(!row||!feedback_is_a(row,"JackUMGItemBase")||read<uint8_t>(row+0x1F8))continue;
            const float y=read<float>(slot+0x4C),height=read<float>(slot+0x54);
            if(height>0&&height<=100&&y>=bottom&&y<spare_y){spare=row;spare_y=y;}
        }
        if(!spare)return; // Never overwrite an existing command.
        int budget=64;const auto label=feedback_find_text(spare,budget);if(!label)throw std::runtime_error("Equipment Lock caption unavailable");
        if(feedback_functions.empty())feedback_bind_umg(label,background,canvas);
        equipment_lock_require_field(read<uintptr_t>(spare+0x10),"bIsSelective",0x370);
        equipment_lock_require_field(read<uintptr_t>(spare+0x10),"DataNumber",0x3BC);
        const auto panel_slot=read<uintptr_t>(canvas+0x38),row_slot=read<uintptr_t>(spare+0x38);
        EquipmentLockEntry entry;entry.owner=weak;entry.row=FeedbackWeak::from(spare);entry.label=FeedbackWeak::from(label);entry.canvas=FeedbackWeak::from(canvas);entry.frame=FeedbackWeak::from(frame);entry.background=FeedbackWeak::from(background);entry.root=FeedbackWeak::from(root);entry.source=source;
        entry.original_visible=read<uint8_t>(spare+0x1F8);entry.original_slate=read<uint8_t>(spare+0x86);entry.original_selectable=read<uint8_t>(spare+0x370);entry.original_active=read<uint8_t>(spare+0x1FA);
        entry.original_background=equipment_lock_height(background);entry.original_frame=equipment_lock_height(frame);entry.original_root=equipment_lock_height(root);entry.original_widget=read<float>(owner+0x260);entry.original_parent=equipment_lock_height(owner);
        entry.required=itemization::equipment_lock_required_height(read<float>(panel_slot+0x4C),spare_y,read<float>(row_slot+0x54),entry.original_background,bottom);
        found=equipment_lock_entries.emplace(key,std::move(entry)).first;
    }
    auto& entry=found->second;const auto row=entry.row.get(),label=entry.label.get();
    if(entry.source!=source){entry.source=source;entry.caption.clear();}
    const auto selected=FeedbackWeak::from(read<uintptr_t>(owner+0x3A0)?read<uintptr_t>(owner+0x3A0):row);
    bool changed=false;
    const std::string caption=itemization::equipment_lock_menu_label(item_service->is_locked(runtime_ids.at(source)));
    if(entry.caption!=caption){feedback_set_text(label,caption);entry.caption=caption;}
    if(!read<uint8_t>(row+0x1F8)||read<uint8_t>(row+0x86)==1||read<uint8_t>(row+0x86)==2){uint8_t visible=1;craft_call(row,"SetWidgetVisible",&visible,1);feedback_visibility(row,0);changed=true;}
    if(!read<uint8_t>(row+0x1FA)){uint8_t active=1;craft_call(row,"SetWidgetActive",&active,1);}
    *reinterpret_cast<uint8_t*>(row+0x370)=1;
    if(!equipment_lock_registered(owner,row)){
        // Native SetItemList discards its selection argument and selects the
        // first command. Preserve the pointer explicitly across registration.
        struct{uintptr_t canvas,item;} list{entry.canvas.get(),selected.get()};craft_call(owner,"SetItemList",&list,16);changed=true;
        if(!equipment_lock_registered(owner,row))throw std::runtime_error("Native spare Lock command was not registered");
    }
    const auto root=entry.root.get(),frame=entry.frame.get(),background=entry.background.get();
    if(equipment_lock_height(root)!=entry.required||equipment_lock_height(background)!=entry.required||equipment_lock_height(owner)!=entry.required||read<float>(owner+0x260)!=entry.required)changed=true;
    equipment_lock_set_height(background,entry.required);equipment_lock_set_height(frame,entry.required);equipment_lock_set_height(root,entry.required);
    *reinterpret_cast<float*>(owner+0x260)=entry.required;
    // Resize the actual clipping parent as well as the decorative frame.
    const auto owner_slot=read<uintptr_t>(owner+0x38);struct{float x,y;} size{read<float>(owner+0x25C),entry.required};
    if(read<float>(owner_slot+0x50)!=size.x||read<float>(owner_slot+0x54)!=size.y)craft_call(owner_slot,"SetSize",&size,8);
    if(changed){craft_call(owner,"ForceLayoutPrepass",nullptr,0);if(const auto item=selected.get())craft_call(owner,"SelectItemFromRef",const_cast<uintptr_t*>(&item),8);}
    if(!entry.ready_logged&&read<uint8_t>(owner+0x86)==0){entry.ready_logged=true;log({{"event","equipment_bag_lock_native_row_ready"},{"source",runtime_ids.at(source)},{"native_row_reused",true},{"registered",equipment_lock_registered(owner,row)},{"data_number",read<int32_t>(row+0x3BC)},{"parent_height",equipment_lock_height(owner)},{"root_height",equipment_lock_height(root)}});}
}
bool equipment_lock_needs_refresh(uintptr_t owner) {
    if(!equipment_lock_command(owner)||craft_ui||!read<uint8_t>(owner+0x1F8)||read<uint8_t>(owner+0x86)!=0)return false;
    const auto weak=FeedbackWeak::from(owner);const auto found=equipment_lock_entries.find({weak.index,weak.serial});
    const auto detail=read<uintptr_t>(owner+0x4A0),canvas=read<uintptr_t>(owner+0x440);
    const auto id=detail&&canvas&&read<uintptr_t>(owner+0x620)==canvas&&read<uint8_t>(detail+0x39)==9?read<uint64_t>(detail+0x48):0;
    if(!id||!runtime_ids.count(id))return found!=equipment_lock_entries.end();
    if(found==equipment_lock_entries.end()||found->second.source!=id)return true;
    const auto& entry=found->second;const auto row=entry.row.get(),root=entry.root.get(),background=entry.background.get(),frame=entry.frame.get();
    if(!row||!root||!background||!frame)return true;
    const auto slot=read<uintptr_t>(row+0x38);
    const bool geometry=equipment_lock_height(root)==entry.required&&equipment_lock_height(owner)==entry.required&&equipment_lock_height(background)==entry.required&&equipment_lock_height(frame)==entry.required&&read<float>(owner+0x260)==entry.required;
    return !itemization::equipment_lock_row_ready(slot&&read<uintptr_t>(slot+0x38)==canvas,read<uint8_t>(row+0x1F8)!=0,read<uint8_t>(row+0x86)!=1&&read<uint8_t>(row+0x86)!=2,read<uint8_t>(row+0x370)!=0, equipment_lock_registered(owner,row),read<uint8_t>(row+0x1FA)!=0,geometry);
}
bool equipment_lock_menu_event(uintptr_t owner,uintptr_t function,const void* params) {
    if(!params||!equipment_lock_command(owner)||read<uint16_t>(function+0x9E)!=16)return false;
    const auto weak=FeedbackWeak::from(owner);const auto found=equipment_lock_entries.find({weak.index,weak.serial});
    if(found==equipment_lock_entries.end()||read<uintptr_t>(reinterpret_cast<uintptr_t>(params)+8)!=found->second.row.get())return false;
    if(read<uint64_t>(reinterpret_cast<uintptr_t>(params))!=read<uint64_t>(base+0x5C1E3B0))return false;
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex);
    const auto source=equipment_lock_source(owner);
    if(!source||!equipment_lock_commit(source))throw std::runtime_error("Equipment Bag selection changed; lock was not changed");
    equipment_lock_add_entry(owner);return true;
}
