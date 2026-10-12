// Native inventory rows and detail cards, separate from the custom crafting
// canvas. Bind to the item identity, never to a shared base-item texture.
struct InventoryRarityLayout {
    int icon=-1,back=-1,detail=-1,id=-1,load_id=-1,frame=-1,frame_detail=-1,frame_soubi=-1;
    bool valid=false;
};
struct InventoryRarityPaint {
    FeedbackWeak owner,back;
    uint64_t item=0;
    bool assigned=false;
    std::array<uint8_t,40> original_brush{};
    std::array<float,4> original_colour{};
};
std::set<uint32_t> inventory_rarity_events;
std::map<uintptr_t,InventoryRarityLayout> inventory_rarity_layouts;
std::map<std::pair<int,int>,InventoryRarityPaint> inventory_rarity_rows;
std::unordered_map<uint64_t,std::string> inventory_rarity_cache;
uint32_t inventory_rarity_draw=0,inventory_rarity_apply=0,inventory_rarity_set=0,inventory_rarity_soubi=0;
using InventoryIconTextureFn=void(*)(void*,void*,bool);
InventoryIconTextureFn original_inventory_icon_texture=nullptr;
using InventoryBackColourFn=void(*)(void*,const void*);
InventoryBackColourFn original_inventory_back_colour=nullptr;
InventoryRarityLayout inventory_rarity_layout(uintptr_t object) {
    const auto cls=read<uintptr_t>(object+0x10);
    if(const auto found=inventory_rarity_layouts.find(cls);found!=inventory_rarity_layouts.end())return found->second;
    InventoryRarityLayout result;
    const auto type=fname(read<uint64_t>(cls+0x18));
    if(type.rfind("WBP_Menu_",0)==0 || type.rfind("WBP_UMG_FusigiKazi_",0)==0) {
        // Resolve named reflected fields once per widget class. No UObject
        // enumeration, widget-tree scans or work on every frame.
        for(auto c=cls;c;c=read<uintptr_t>(c+0x40))
            for(auto f=read<uintptr_t>(c+0x48);f;f=read<uintptr_t>(f+0x38)) {
                const auto kind=fname(read<uint64_t>(read<uintptr_t>(f+0x10)+0x18));
                const auto name=fname(read<uint64_t>(f+0x18));const auto offset=read<int>(f+0x54);
                if(kind=="StructProperty"&&name=="UberGraphFrame")result.frame=offset;
                if(kind=="Function"&&name.rfind("ExecuteUbergraph_",0)==0) {
                    for(auto p=read<uintptr_t>(f+0x48);p;p=read<uintptr_t>(p+0x38)) {
                        const auto pn=fname(read<uint64_t>(p+0x18));
                        const auto pk=fname(read<uint64_t>(read<uintptr_t>(p+0x10)+0x18));
                        if(pk=="ObjectProperty"&&pn=="K2Node_CustomEvent_InItemSyousai")result.frame_detail=read<int>(p+0x54);
                        if(type=="WBP_Menu_Soubi_Item_000_modern_C"&&pk=="StructProperty"&&pn=="K2Node_CustomEvent_Data"&&read<int>(p+0x44)==112)
                            result.frame_soubi=read<int>(p+0x54);
                    }
                }
                if(kind=="ObjectProperty") {
                    if(name=="ItemIconPanel"||name=="IconImagePanel"||name=="IconPanel"||name=="SmallIconPanel")result.icon=offset;
                    if(name=="ItemBasePanel"||name=="Zabuton")result.back=offset;
                    if(name=="ItemSyousaiRef")result.detail=offset;
                } else {
                    if(name=="ItemID")result.id=offset;
                    if(name=="LoadIconItemId")result.load_id=offset;
                }
            }
        result.valid=result.icon>=0;
    }
    inventory_rarity_layouts.emplace(cls,result);return result;
}
std::string inventory_rarity(uint64_t id) {
    if(!id)return {};
    if(const auto found=inventory_rarity_cache.find(id);found!=inventory_rarity_cache.end())return found->second;
    std::string rarity;
    if(const auto generated=runtime_ids.find(id);generated!=runtime_ids.end()) {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        rarity=item_service->lookup(generated->second).at("rarity").get<std::string>();
    } else {
        const auto name=fname(id);
        if(runtime_bases.count(name)||name.rfind("W_",0)==0||name.rfind("E_",0)==0||name.rfind("A_",0)==0)rarity="normal";
    }
    // Unknown IDs can be registered later by acquisition. Do not retain a miss.
    if(!rarity.empty())inventory_rarity_cache.emplace(id,rarity);
    return rarity;
}
void inventory_rarity_prepare(uintptr_t object,uintptr_t function,void* params,uint32_t event) {
    const auto weak=FeedbackWeak::from(object);if(weak.get()!=object)return;
    const auto layout=inventory_rarity_layout(object);if(!layout.valid)return;
    auto& paint=inventory_rarity_rows[{weak.index,weak.serial}];paint.owner=weak;
    // Capture the exact supplied identity before nested/async texture loading.
    // This also clears a recycled row when DrawItemBase receives no item.
    if(params&&(event==inventory_rarity_draw||event==inventory_rarity_apply)&&read<uint16_t>(function+0x9E)==8) {
        const auto data=read<uintptr_t>(reinterpret_cast<uintptr_t>(params));
        paint.item=0;paint.assigned=true;
        if(data&&feedback_is_a(data,"JackUMGItemData")) {
            feedback_require_property(read<uintptr_t>(data+0x10),"ItemID",0x40);
            paint.item=read<uint64_t>(data+0x40);
        }
    } else if(params&&event==inventory_rarity_set&&read<uint16_t>(function+0x9E)==28) {
        feedback_require_property(function,"InItemId",8);
        paint.item=read<uint64_t>(reinterpret_cast<uintptr_t>(params)+8);paint.assigned=true;
    } else if(params&&event==inventory_rarity_soubi&&read<uint16_t>(function+0x9E)==113) {
        feedback_require_property(function,"Data",0);
        paint.item=read<uint64_t>(reinterpret_cast<uintptr_t>(params)+0x10);paint.assigned=true;
    }
}
void inventory_rarity_refresh(uintptr_t object) {
    const auto weak=FeedbackWeak::from(object);if(weak.get()!=object)return;
    const auto layout=inventory_rarity_layout(object);if(!layout.valid)return;
    auto& paint=inventory_rarity_rows[{weak.index,weak.serial}];paint.owner=weak;
    const auto icon=read<uintptr_t>(object+layout.icon);
    if(!icon||!feedback_is_a(icon,"Image"))return;
    uint64_t id=paint.assigned?paint.item:0;bool resolved=paint.assigned;
    // Blueprint-to-Blueprint calls need not enter ProcessEvent. The reflected
    // persistent frame retains the exact selected data through async loading.
    // These two native widgets otherwise do not retain an ItemSyousaiRef.
    if(layout.frame>=0)if(const auto frame=read<uintptr_t>(object+layout.frame)) {
        if(layout.frame_soubi>=0){id=read<uint64_t>(frame+layout.frame_soubi+0x10);resolved=true;}
        else if(layout.frame_detail>=0) {
            id=0;resolved=true;
            if(const auto detail=read<uintptr_t>(frame+layout.frame_detail))
                if(feedback_is_a(detail,"JackUMGItemSyousai"))id=read<uint64_t>(detail+0x48);
        }
    }
    if(!resolved) {
        if(layout.detail>=0)if(const auto detail=read<uintptr_t>(object+layout.detail))
            if(feedback_is_a(detail,"JackUMGItemSyousai")) {
                feedback_require_property(read<uintptr_t>(detail+0x10),"ItemID",0x48);id=read<uint64_t>(detail+0x48);
            }
        if(!id&&layout.load_id>=0)id=read<uint64_t>(object+layout.load_id);
        if(!id&&layout.id>=0)id=read<uint64_t>(object+layout.id);
    }
    const auto rarity=inventory_rarity(id);
    uintptr_t back=layout.back<0?0:read<uintptr_t>(object+layout.back);
    if(back&&!feedback_is_a(back,"Image"))back=0;
    if(!back)back=paint.back.get();
    bool added=false;
    if(!back) {
        if(rarity.empty())return;
        const auto slot=read<uintptr_t>(icon+0x38);const auto tree=read<uintptr_t>(object+0x168);
        if(!slot||!tree||!feedback_is_a(slot,"CanvasPanelSlot"))return;
        const auto canvas=read<uintptr_t>(slot+0x38);if(!canvas||!feedback_is_a(canvas,"CanvasPanel"))return;
        // Some native selectors have an icon without a backplate. Add a
        // non-interactive image in the same canvas, with its exact anchors.
        back=reinterpret_cast<FeedbackConstructFn>(base+0xF16220)(read<uintptr_t>(icon+0x10),tree,native_name("ARPG_InventoryRarity"),0,0,0,false,0,false);
        if(!back||read<uintptr_t>(back+0x20)!=tree)throw std::runtime_error("Native rarity backplate construction failed");
        struct {uintptr_t child,result;} child{back,0};craft_call(canvas,"AddChildToCanvas",&child,16);
        if(!child.result)throw std::runtime_error("Native rarity backplate attachment failed");
        craft_texture(back,craft_asset("/Game/UIs_modern/Texture/T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002"));
        std::array<float,10> rect{};std::memcpy(rect.data(),reinterpret_cast<void*>(slot+0x48),40);
        craft_call(child.result,"SetLayout",rect.data(),40);
        // Alignment is already included in the 40-byte AnchorData above.
        int order=read<int>(slot+0x74)-1;craft_call(child.result,"SetZOrder",&order,4);
        uint8_t visibility=3;craft_call(back,"SetVisibility",&visibility,1);
        added=true;
    }
    if(paint.back.get()!=back) {
        paint.back=FeedbackWeak::from(back);
        // Retain the specified colour, not a borrowed Slate shared reference.
        std::memcpy(paint.original_brush.data(),reinterpret_cast<void*>(back+0xE8),16);
        std::memcpy(paint.original_colour.data(),reinterpret_cast<void*>(back+0x150),16);
        if(added)paint.original_colour={0,0,0,0};
        log({{"event","native_inventory_rarity_bound"},{"class",fname(read<uint64_t>(read<uintptr_t>(object+0x10)+0x18))},{"owner_index",weak.index},{"owner_serial",weak.serial}});
        for(auto it=inventory_rarity_rows.begin();it!=inventory_rarity_rows.end();)
            if(!it->second.owner.get())it=inventory_rarity_rows.erase(it);else ++it;
    }
    // Restore the native appearance for empty cells, consumables and currency.
    // Only the backplate changes; artwork, disabled state and focus are native.
    auto tint=paint.original_brush;auto colour=paint.original_colour;
    if(!rarity.empty()) {
        const std::array<float,4> white{1,1,1,1};std::memcpy(tint.data(),white.data(),16);
        colour=craft_rarity_tint(rarity);
    }
    // This owner build exposes SetBrush, not newer UImage tint setters.
    // Keep resource, margins, tiling and dimensions from the live brush.
    if(std::memcmp(reinterpret_cast<void*>(back+0xE8),tint.data(),16)!=0) {
        std::array<uint8_t,120> brush{};std::memcpy(brush.data(),reinterpret_cast<void*>(back+0xC8),120);
        std::memcpy(brush.data()+0x20,tint.data(),40);craft_call(back,"SetBrush",brush.data(),120);
    }
    if(std::memcmp(reinterpret_cast<void*>(back+0x150),colour.data(),16)!=0)craft_call(back,"SetColorAndOpacity",colour.data(),16);
}
void inventory_rarity_event(uintptr_t object,uintptr_t function,void* params,uint32_t event,bool before) noexcept {
    if(!inventory_rarity_events.count(event))return;
    try {if(before)inventory_rarity_prepare(object,function,params,event);else inventory_rarity_refresh(object);}
    catch(const std::exception& e) {static std::set<std::string> seen;if(seen.insert(e.what()).second)log({{"event","native_inventory_rarity_refused"},{"reason",e.what()}});}
}
void inventory_icon_texture_hook(void* image,void* texture,bool resize) {
    original_inventory_icon_texture(image,texture,resize);
    if(craft_internal_event||!runtime_enabled.load())return;
    try {
        const auto icon=reinterpret_cast<uintptr_t>(image);
        const auto tree=read<uintptr_t>(icon+0x20);
        if(!tree||!feedback_is_a(tree,"WidgetTree"))return;
        const auto owner=read<uintptr_t>(tree+0x20);if(!owner)return;
        const auto layout=inventory_rarity_layout(owner);
        if(layout.valid&&read<uintptr_t>(owner+layout.icon)==icon)inventory_rarity_refresh(owner);
    } catch(const std::exception& e) {static std::set<std::string> seen;if(seen.insert(e.what()).second)log({{"event","native_inventory_icon_rarity_refused"},{"reason",e.what()}});}
}
void inventory_back_colour_hook(void* image,const void* colour) {
    original_inventory_back_colour(image,colour);
    if(craft_internal_event||!runtime_enabled.load())return;
    try {
        const auto back=reinterpret_cast<uintptr_t>(image);const auto tree=read<uintptr_t>(back+0x20);
        if(!tree||!feedback_is_a(tree,"WidgetTree"))return;
        const auto owner=read<uintptr_t>(tree+0x20);if(!owner)return;
        const auto layout=inventory_rarity_layout(owner);
        // Native row restyling also occurs when the next item has the same
        // icon asset. Its backplate update must repaint the new identity.
        if(layout.valid&&layout.back>=0&&read<uintptr_t>(owner+layout.back)==back)inventory_rarity_refresh(owner);
    } catch(const std::exception& e) {static std::set<std::string> seen;if(seen.insert(e.what()).second)log({{"event","native_inventory_back_rarity_refused"},{"reason",e.what()}});}
}
void inventory_rarity_initialize() {
    inventory_rarity_draw=static_cast<uint32_t>(native_name("DrawItemBase"));
    inventory_rarity_apply=static_cast<uint32_t>(native_name("ApplyItemData"));
    inventory_rarity_set=static_cast<uint32_t>(native_name("SetItemData"));
    inventory_rarity_soubi=static_cast<uint32_t>(native_name("SetSoubiData"));
    for(const char* event:{"DrawItemBase","ApplyItemData","SetItemData","InitItem","SetSoubiData","SetItemSyousai",
        "SetDouguMenuDescription","SetDouguMenuDescription1","SetShopMenuDescription1","SetShopMenuDescription2","SetSetumeiCaptionAndIcon",
        "LoadTextureComplete","LoadCompleteItemIcon","LoaddedMenuItemIcon","LoaddedSetBookIcon","CustomEvent_0","NotificationItemBase"})
        inventory_rarity_events.insert(static_cast<uint32_t>(native_name(event)));
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x16B0A00),reinterpret_cast<void*>(&inventory_icon_texture_hook),reinterpret_cast<void**>(&original_inventory_icon_texture))!=MH_OK ||
       MH_EnableHook(reinterpret_cast<void*>(base+0x16B0A00))!=MH_OK)throw std::runtime_error("Native inventory rarity hook failed");
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x16B0F10),reinterpret_cast<void*>(&inventory_back_colour_hook),reinterpret_cast<void**>(&original_inventory_back_colour))!=MH_OK ||
       MH_EnableHook(reinterpret_cast<void*>(base+0x16B0F10))!=MH_OK)throw std::runtime_error("Native rarity backplate refresh hook failed");
}
