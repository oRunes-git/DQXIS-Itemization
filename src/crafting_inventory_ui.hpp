// Included after the independent page's reflected-call and text helpers.
struct CraftAsset {FeedbackWeak object;uint64_t retain_until=0;};
std::map<std::string,CraftAsset> craft_assets;
bool craft_texture_valid(uintptr_t texture) {
    if(!texture)return false;
    return FeedbackWeak::from(texture).get()==texture && feedback_is_a(texture,"Texture2D");
}
uintptr_t craft_asset(const std::string& path) {
    const auto now=GetTickCount64();
    if(const auto found=craft_assets.find(path);found!=craft_assets.end())
        if(const auto value=found->second.object.get())if(now<found->second.retain_until && craft_texture_valid(value))return value;
    // Same FName asset loader as GetItemPopIconTexture: cached resolution first,
    // native package load if needed. No UObject enumeration during navigation.
    auto name=native_name(path);
    // Positive lifetime adds the texture to Jack's GC-traced asset cache.
    // A weak C++ reference alone does not keep artwork alive between pages.
    // Renew before the native one-hour lease expires, without navigation scans.
    const auto object=reinterpret_cast<uintptr_t(*)(uintptr_t,const uint64_t*,float,int)>(base+0x7FFA20)(0,&name,3600.0f,0);
    if(!craft_texture_valid(object))throw std::runtime_error("Crafting artwork is not a live Texture2D: "+path);
    craft_assets[path]={FeedbackWeak::from(object),now+1800000};return object;
}
void craft_texture(uintptr_t image,uintptr_t texture) {
    if(!image)throw std::runtime_error("Crafting image is unavailable");
    if(texture&&!craft_texture_valid(texture))throw std::runtime_error("Crafting image received a non-texture object");
    struct {uintptr_t texture;uint8_t resize;} p{texture,0};craft_call(image,"SetBrushFromTexture",&p,9);
}
void craft_texture_fit(uintptr_t image,uintptr_t texture,float x,float y,float w,float h) {
    if(!image||!texture)return;
    if(!craft_texture_valid(texture))throw std::runtime_error("Crafting fitted image received a non-texture object");
    // Do not ask SetBrushFromTexture to dereference PlatformData implicitly.
    // The crash occurred inside that unchecked native resize branch for Bag.
    // Read dimensions only from committed readable platform data.
    float native_w=0,native_h=0;const auto platform=read<uintptr_t>(texture+0x108);
    MEMORY_BASIC_INFORMATION memory{};
    if(platform&&VirtualQuery(reinterpret_cast<void*>(platform),&memory,sizeof(memory)) && memory.State==MEM_COMMIT &&
       !(memory.Protect&(PAGE_NOACCESS|PAGE_GUARD)) && platform>=reinterpret_cast<uintptr_t>(memory.BaseAddress) &&
       platform-reinterpret_cast<uintptr_t>(memory.BaseAddress)+8<=memory.RegionSize) {
        native_w=static_cast<float>(read<int32_t>(platform));native_h=static_cast<float>(read<int32_t>(platform+4));
    }
    craft_texture(image,texture);
    if(native_w>0 && native_h>0 && native_w<=16384 && native_h<=16384 && std::isfinite(native_w) && std::isfinite(native_h)) {
        const float ratio=std::min(w/native_w,h/native_h);const float width=native_w*ratio,height=native_h*ratio;
        feedback_rect(image,x+(w-width)/2,y+(h-height)/2,width,height);
    } else feedback_rect(image,x,y,w,h);
}
void craft_available_text(CraftText& text,bool available) {
    std::array<uint8_t,40> colour{};const std::array<float,4> tint=available?
        std::array<float,4>{1,1,1,1}:std::array<float,4>{0.38f,0.31f,0.22f,1};
    std::memcpy(colour.data(),tint.data(),16);craft_call(text.widget.get(),"SetColorAndOpacity",colour.data(),40);
}
const itemization::CraftItemPresentation& craft_selected_presentation(CraftUI& ui) {
    const auto& id=ui.inventory.selected().id;ui.presentation_id=id;
    auto found=ui.presentations.find(id);
    if(found==ui.presentations.end()) {
        itemization::CraftItemPresentation view;
        if(!id.empty()) {
            const auto key=native_name(id);const auto generated=runtime_ids.find(key);
            if(generated!=runtime_ids.end())view=itemization::crafting_generated_presentation(*item_service,item_service->lookup(generated->second));
            else if(const auto base=runtime_bases.find(id);base!=runtime_bases.end())view=itemization::crafting_native_presentation(base->second);
        }
        found=ui.presentations.emplace(id,std::move(view)).first;
    }
    return found->second;
}
uintptr_t craft_art(uintptr_t tree,uintptr_t canvas,const std::string& name,const std::string& path,
    float x,float y,float w,float h,bool box=false,bool header=false,const std::array<float,4>& tint={1,1,1,1}) {
    const auto image=feedback_image(tree,canvas,name,tint);craft_texture(image,craft_asset(path));
    if(box) {
        std::array<uint8_t,120> brush{};std::memcpy(brush.data(),reinterpret_cast<void*>(image+0xC8),120);
        const std::array<float,4> margins{0.4f,header?0.0f:0.4f,0.4f,header?0.0f:0.4f};
        std::memcpy(brush.data()+0x10,margins.data(),16);brush[0x6C]=1;
        craft_call(image,"SetBrush",brush.data(),120);
    }feedback_rect(image,x,y,w,h);return image;
}
void craft_pale_text(CraftText& text) {
    std::array<uint8_t,40> colour{};const std::array<float,4> brown{0.16f,0.075f,0.035f,1};
    std::memcpy(colour.data(),brown.data(),16);craft_call(text.widget.get(),"SetColorAndOpacity",colour.data(),40);
    std::array<float,4> transparent{};craft_call(text.widget.get(),"SetBorderColorAndOpacity",transparent.data(),16);
    craft_call(text.widget.get(),"SetShadowColorAndOpacity",transparent.data(),16);
}
std::array<float,4> craft_rarity_tint(const std::string& rarity) {
    if(rarity=="magic")return {0.12f,0.42f,1.0f,0.45f};
    if(rarity=="rare")return {1.0f,0.80f,0.08f,0.45f};
    return {0.65f,0.65f,0.65f,0.28f};
}
std::vector<int> craft_party(uintptr_t accessor) {
    std::vector<int> party;
    for(uint8_t c=0;c<8;++c){struct {uint8_t character,result;} p{c,0};craft_call(accessor,"IsPartyMemberFromType",&p,2);if(p.result)party.push_back(c);}
    return party;
}
void craft_inventory_refresh(CraftUI& ui,const std::string& preferred={}) {
    const bool salvage=ui.model->kind()==itemization::CraftScreenKind::Salvage;
    const auto party=salvage?std::vector<int>{}:craft_party(ui.members_accessor.get());std::vector<itemization::CraftInventorySlot> slots;
    std::vector<itemization::CraftEquipmentAssignment> assignments;
    if(!salvage&&!original_equipped)throw std::runtime_error("Crafting equipped-item accessor unavailable");
    for(const auto character:party) {
        struct {uint8_t character;uint8_t padding[7];uintptr_t result;} p{static_cast<uint8_t>(character),{},0};
        craft_call(ui.members_accessor.get(),"GetJackCharacterFromType",&p,16);
        if(!p.result)throw std::runtime_error("Crafting current party character unavailable");
        // Native getter returns its existing array. Do not invoke grants, test
        // callbacks or allocate/copy an Unreal-owned reflected return array.
        const auto list=read<Array>(reinterpret_cast<uintptr_t>(original_equipped(reinterpret_cast<void*>(p.result))));
        if(list.count<0||list.count>8||list.capacity<list.count)throw std::runtime_error("Crafting equipped-item array invalid");
        for(int i=0;i<list.count;++i){const auto slot=list.data+uintptr_t(i)*16;
            assignments.push_back({fname(read<uint64_t>(slot)),character,read<uint8_t>(slot+8)});}
    }
    const auto manager=find_manager();std::set<std::string> craftable;
    for(const auto& g:ui.model->gear())craftable.insert(g.at("engine_id"));
    for(int bag=salvage?itemization::salvage_equipment_bag:0;bag<=9;++bag) {
        if(bag==8 || (bag<8 && std::find(party.begin(),party.end(),bag)==party.end()))continue;
        const auto list=read<Array>(manager+0x128+uintptr_t(bag)*0x30);
        if(list.count<0||list.count>10000||list.capacity<list.count)throw std::runtime_error("Crafting grid inventory invalid");
        for(int i=0;i<list.count;++i) {
            const auto payload=read<uintptr_t>(list.data+uintptr_t(i)*16);if(!payload||read<int32_t>(payload+0x18)<1)continue;
            const auto key=read<uint64_t>(payload+0x10);const auto id=fname(key);
            const auto definition=runtime_bases.find(id);const bool generated=runtime_ids.count(key)!=0;
            if(!generated&&definition==runtime_bases.end())continue; // Consumables never enter the equipment grid.
            if(salvage&&generated&&item_service->is_locked(id))continue; // No visible or selectable locked rows.
            std::string name=id,rarity="normal";
            if(generated){const auto item=item_service->lookup(runtime_ids.at(key));name=itemization::instance_name(item);rarity=item.at("rarity");}
            else name=definition->second.at("display_name");
            slots.push_back({id,name,bag<8?bag:-1,0,craftable.count(id)!=0,rarity});
        }
    }
    ui.inventory.refresh(party,itemization::crafting_inventory_slots(slots,assignments),preferred);
    ui.can_craft=ui.inventory.selected().craftable&&ui.model->select_source(ui.inventory.selected().id);
}
uintptr_t craft_item_icon(CraftUI& ui,uint64_t id,int character,int region) {
    if(id) {
        const auto cached=ui.icon_cache.find(id);if(cached!=ui.icon_cache.end())if(const auto texture=cached->second.get())return texture;
        struct {uint64_t id;uintptr_t result;} p{id,0};craft_call(ui.item_accessor.get(),"GetItemPopIconTexture",&p,16);
        if(p.result)ui.icon_cache[id]=FeedbackWeak::from(p.result);return p.result;
    }
    if(character<0)return 0;
    struct {uint8_t character,region,item;uint8_t padding[5];uintptr_t result;} p{static_cast<uint8_t>(character),static_cast<uint8_t>(region),1,{},0};
    craft_call(ui.item_accessor.get(),"GetNoEquipmentIcon",&p,16);return p.result;
}
void craft_grid_render(CraftUI& ui) {
    const auto& grid=ui.inventory;const bool equipment=ui.model->stage()==itemization::CraftScreenStage::Equipment;
    const bool salvage=ui.model->kind()==itemization::CraftScreenKind::Salvage;
    for(size_t c=0;c<9;++c) {
        const bool visible=c<=grid.party().size();
        if(const auto frame=ui.portrait_frames[c].get())feedback_visibility(frame,!salvage&&visible?3:1);
        if(const auto divider=ui.column_dividers[c].get())feedback_visibility(divider,!salvage&&visible&&c>0?3:1);
        if(c<8) {
            const auto portrait=ui.portraits[c].get();if(portrait)feedback_visibility(portrait,c<grid.party().size()?3:1);
            if(c<grid.party().size()&&ui.portrait_characters[c]!=grid.party()[c]) {struct {uint8_t character,type;uint8_t padding[6];uintptr_t result;} p{static_cast<uint8_t>(grid.party()[c]),6,{},0};
                craft_call(ui.members_accessor.get(),"GetCharacterFaceIconFromType",&p,16);if(p.result){craft_texture_fit(portrait,p.result,64+static_cast<float>(c)*120,164,98,64);ui.portrait_characters[c]=grid.party()[c];}}
        }
        for(size_t r=0;r<6;++r) {
            auto& cell=ui.slots[c][r];const auto icon=cell.icon.get();if(!icon)continue;
            const auto& slot=grid.at(c,r);
            const bool occupied=visible&&(!slot.id.empty()||c<grid.party().size());
            feedback_visibility(icon,occupied?3:1);
            feedback_visibility(cell.marker.widget.get(),occupied?3:1);
            feedback_visibility(cell.background.get(),occupied?3:1);
            feedback_visibility(cell.rarity.get(),occupied&&!slot.id.empty()?3:1);
            if(salvage)feedback_visibility(ui.salvage_names[r].widget.get(),occupied?3:1);
            if(!occupied)continue;
            if(!slot.id.empty()&&cell.previous_rarity!=slot.rarity) {
                auto tint=craft_rarity_tint(slot.rarity);craft_call(cell.rarity.get(),"SetColorAndOpacity",tint.data(),16);
                cell.previous_rarity=slot.rarity;
            }
            const uint64_t id=slot.id.empty()?0:native_name(slot.id);
            // Include empty-slot identity in the cache key, so changing party
            // columns never retains another character's hand placeholder.
            const uint64_t signature=id?id:(uint64_t(slot.character+2)<<32)|uint64_t(slot.region);
            if(signature!=cell.previous_icon) {
                const auto texture=craft_item_icon(ui,id,slot.character,slot.region);craft_texture(icon,texture);cell.previous_icon=signature;
            }
            craft_text_set(cell.marker,ui.model->kind()==itemization::CraftScreenKind::Salvage&&ui.model->selected(slot.id)?"Selected":
                id&&runtime_ids.count(id)&&item_service->is_locked(runtime_ids.at(id))?"Locked":"");
            if(salvage) {
                craft_text_set(ui.salvage_names[r],slot.name);
                const int enabled=slot.craftable?1:0;
                if(ui.salvage_enabled[r]!=enabled) {craft_available_text(ui.salvage_names[r],enabled);ui.salvage_enabled[r]=enabled;}
            }
        }
    }
    const float bag_x=64+static_cast<float>(grid.party().size())*120;
    const auto bag_icon=ui.bag_icon.get();
    if(salvage) {
        feedback_rect(ui.gear_title.widget.get(),174,174,920,42);craft_text_set(ui.gear_title,"Equipment Bag");
    }else if(!bag_icon || read<uint8_t>(bag_icon+0x86)==1) {
        feedback_rect(ui.gear_title.widget.get(),bag_x,174,98,42);craft_text_set(ui.gear_title,"Bag");
    }else craft_text_set(ui.gear_title,"");
    feedback_visibility(ui.bag_track.get(),grid.bag().size()>6?3:1);feedback_visibility(ui.bag_thumb.get(),grid.bag().size()>6?3:1);
    if(grid.bag().size()>6) {
        feedback_rect(ui.bag_track.get(),salvage?1146:bag_x+104,238,6,404);
        const float thumb=404*6.0f/static_cast<float>(grid.bag().size());
        const float top=238+(404-thumb)*static_cast<float>(grid.bag_first())/static_cast<float>(grid.bag().size()-6);
        feedback_rect(ui.bag_thumb.get(),salvage?1146:bag_x+104,top,6,thumb);
    }
    feedback_rect(ui.selection.get(),60+static_cast<float>(grid.column())*120,236+static_cast<float>(grid.visible_row())*68,salvage?1078:104,68);
    feedback_visibility(ui.selection.get(),equipment&&!grid.selected().id.empty()?3:1);
    const auto& selected=grid.selected();craft_text_set(ui.selected_name,selected.id.empty()?(salvage?"Equipment Bag is empty":"Empty slot"):selected.name);
    const uint64_t selected_id=selected.id.empty()?0:native_name(selected.id);
    feedback_visibility(ui.selected_rarity.get(),selected_id?3:1);
    if(selected_id&&ui.previous_selected_rarity!=selected.rarity) {
        auto tint=craft_rarity_tint(selected.rarity);craft_call(ui.selected_rarity.get(),"SetColorAndOpacity",tint.data(),16);
        ui.previous_selected_rarity=selected.rarity;
    }
    feedback_visibility(ui.selected_icon.get(),selected_id||(!salvage&&selected.character>=0)?3:1);
    if(salvage) {
        const uint8_t visibility=selected_id?3:1;
        for(const auto& frame:ui.badge_frames)feedback_visibility(frame.get(),visibility);
        for(auto* badge:{&ui.rarity_badge,&ui.class_badge,&ui.quality_badge})feedback_visibility(badge->widget.get(),visibility);
    }
    const auto selected_signature=selected_id?selected_id:(uint64_t(selected.character+2)<<32)|uint64_t(selected.region);
    if(selected_signature!=ui.selected_icon_id){craft_texture(ui.selected_icon.get(),craft_item_icon(ui,selected_id,selected.character,selected.region));ui.selected_icon_id=selected_signature;}
    const auto& view=craft_selected_presentation(ui);
    craft_text_set(ui.rarity_badge,"Rarity: "+view.rarity);
    craft_text_set(ui.class_badge,"Class: "+view.item_class);
    craft_text_set(ui.quality_badge,"Quality: "+view.quality);
}
void craft_grid_create(CraftUI& ui,uintptr_t tree,uintptr_t page,uintptr_t font) {
    const bool salvage=ui.model->kind()==itemization::CraftScreenKind::Salvage;
    const std::string modern="/Game/UIs_modern/Texture/",common="/Game/UIs_common/Texture/";
    craft_art(tree,page,"ARPG_CraftingEquipmentPaper",modern+"T_Hud_Modern_Paper_018.T_Hud_Modern_Paper_018",0,0,1920,1080);
    craft_art(tree,page,"ARPG_CraftingTitleFrame",common+"T_Hud_Parts_007.T_Hud_Parts_007",40,40,1110,108,true,true);
    craft_art(tree,page,"ARPG_CraftingStatsFrame",common+"T_Hud_Parts_004.T_Hud_Parts_004",40,766,1120,274,true);
    craft_art(tree,page,"ARPG_CraftingCurrencyFrame",modern+"T_Hud_Modern_Parts_003.T_Hud_Modern_Parts_003",1190,150,682,salvage?520:644,true);
    craft_art(tree,page,"ARPG_CraftingActionFrame",common+"T_Hud_Parts_004.T_Hud_Parts_004",1190,salvage?694:822,682,salvage?346:218,true);
    if(salvage)craft_art(tree,page,"ARPG_SalvageBagHeaderFrame",modern+"T_Hud_Modern_Parts_003.T_Hud_Modern_Parts_003",48,154,1098,76,true);
    ui.selected_rarity=FeedbackWeak::from(craft_art(tree,page,"ARPG_SelectedRarity",modern+"T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002",56,658,78,66,false,false,{0,0,0,0}));
    const auto selected_icon=feedback_image(tree,page,"ARPG_CraftingSelectedIcon",{1,1,1,1});feedback_rect(selected_icon,64,660,62,62);ui.selected_icon=FeedbackWeak::from(selected_icon);
    ui.bag_track=FeedbackWeak::from(feedback_image(tree,page,"ARPG_CraftingBagScrollTrack",{0.15f,0.06f,0.01f,0.35f}));
    ui.bag_thumb=FeedbackWeak::from(feedback_image(tree,page,"ARPG_CraftingBagScrollThumb",{0.95f,0.83f,0.53f,0.9f}));
    const auto bag=feedback_image(tree,page,"ARPG_CraftingBagIcon",{1,1,1,1});ui.bag_icon=FeedbackWeak::from(bag);
    int32_t header_z=2;craft_call(read<uintptr_t>(bag+0x38),"SetZOrder",&header_z,4);
    uintptr_t bag_texture=0;
    // The native accessor only returns portraits for character bags (0..7).
    // Common/equipment bags legitimately return null. Artwork is optional UI,
    // so an unavailable texture must never abort or repeatedly reopen a page.
    try {
        bag_texture=craft_asset("/Game/UIs_common/Texture/Icon/Dougu/T_Hud_Icon_Item_BAG_00001.T_Hud_Icon_Item_BAG_00001");
    }catch(const std::exception& e){log({{"event","crafting_optional_artwork_missing"},{"asset","equipment_bag"},{"reason",e.what()}});}
    if(bag_texture) {
        craft_texture_fit(bag,bag_texture,64+static_cast<float>(ui.inventory.party().size())*120,164,98,64);
        log({{"event","crafting_bag_artwork_bound"},{"live_texture2d",true},{"native_cache_retention_seconds",3600},{"native_resize",false}});
    }
    else feedback_visibility(bag,1);
    for(int i=0;i<3;++i)ui.badge_frames[i]=FeedbackWeak::from(craft_art(tree,page,"ARPG_CraftingBadgeFrame"+std::to_string(i),common+"T_Hud_Parts_004.T_Hud_Parts_004",64+static_cast<float>(i)*292,716,280,44,true));
    // Build only the current roster and bag. A story roster change is reflected
    // on the next open, without constructing six invisible columns now.
    for(size_t c=0;c<=ui.inventory.party().size();++c) {
        const auto x=64+static_cast<float>(c)*120;
        if(!salvage) {
            ui.portrait_frames[c]=FeedbackWeak::from(craft_art(tree,page,"ARPG_PortraitFrame"+std::to_string(c),
                modern+"T_Hud_Modern_Parts_003.T_Hud_Modern_Parts_003",x-6,158,110,76,true));
            if(c>0)ui.column_dividers[c]=FeedbackWeak::from(craft_art(tree,page,"ARPG_ColumnDivider"+std::to_string(c),
                common+"T_Hud_Parts_004.T_Hud_Parts_004",x-12,238,2,404,false,false,{0.44f,0.24f,0.08f,0.45f}));
        }
        if(c<ui.inventory.party().size()){const auto image=feedback_image(tree,page,"ARPG_PartyPortrait"+std::to_string(c),{1,1,1,1});feedback_rect(image,x,164,98,64);ui.portraits[c]=FeedbackWeak::from(image);
            craft_call(read<uintptr_t>(image+0x38),"SetZOrder",&header_z,4);}
        for(size_t r=0;r<6;++r) {
            const auto name="ARPG_InventorySlot"+std::to_string(c)+"_"+std::to_string(r);const float y=238+static_cast<float>(r)*68;
            // Backplates and highlights use native textures, including their soft edges.
            ui.slots[c][r].background=FeedbackWeak::from(craft_art(tree,page,name+"Back",modern+"T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002",x,y,salvage?1070:98,64,false,false,{0.05448f,0.0137f,0,0.6f}));
            ui.slots[c][r].rarity=FeedbackWeak::from(craft_art(tree,page,name+"Rarity",modern+"T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002",x+8,y,78,64,false,false,{0,0,0,0}));
            const auto icon=feedback_image(tree,page,name,{1,1,1,1});feedback_rect(icon,x+16,y+1,62,62);ui.slots[c][r].icon=FeedbackWeak::from(icon);
            ui.slots[c][r].marker=craft_text_create(tree,page,font,name+"Selected",salvage?1030:x,salvage?y+24:y+44,98,20,14);
            if(salvage)ui.salvage_names[r]=craft_text_create(tree,page,font,name+"Name",158,y+14,856,50,24);
        }
    }
    // Highlight is drawn under text/icons via canvas ZOrder, preserving the icon artwork.
    const auto selection=craft_art(tree,page,"ARPG_CraftingSelection",modern+"T_Hud_Modern_Parts_002.T_Hud_Modern_Parts_002",60,236,104,68,false,false,{1,0.76f,0.22f,0.42f});
    ui.selection=FeedbackWeak::from(selection);int32_t z=1;craft_call(read<uintptr_t>(selection+0x38),"SetZOrder",&z,4);
    for(auto& column:ui.slots)for(auto& slot:column)if(const auto icon=slot.icon.get()){z=2;craft_call(read<uintptr_t>(slot.rarity.get()+0x38),"SetZOrder",&z,4);craft_call(read<uintptr_t>(icon+0x38),"SetZOrder",&z,4);craft_call(read<uintptr_t>(slot.marker.widget.get()+0x38),"SetZOrder",&z,4);}
    const auto highlight=feedback_image(tree,page,"ARPG_CraftingCurrencySelection",{1,0.76f,0.22f,0.16f});ui.choice_selection=FeedbackWeak::from(highlight);
    feedback_rect(highlight,1210,226,638,43);z=-1; // Place above frame, below currency text (created next).
    (void)z;
    // The hosting window's cursor is independently attached to the game's root
    // and its visibility is refreshed by Jack. Hide its artwork container, not
    // just that outer widget. Copy the native arrow brush into our own canvas so
    // the arrow follows our exact equipment/currency focus instead.
    feedback_require_property(read<uintptr_t>(ui.host.get()+0x10),"CursorWidget",0x3B8);
    if(const auto cursor=read<uintptr_t>(ui.host.get()+0x3B8)) {
        int budget=64;const auto arrow_image=craft_find_named(cursor,"Enable",budget);
        if(arrow_image&&feedback_is_a(arrow_image,"Image")) {
            const auto arrow=feedback_image(tree,page,"ARPG_CraftingCursor",{1,1,1,1});
            std::array<uint8_t,120> brush{};std::memcpy(brush.data(),reinterpret_cast<void*>(arrow_image+0xC8),120);
            craft_call(arrow,"SetBrush",brush.data(),120);ui.selection_arrow=FeedbackWeak::from(arrow);
            z=3;craft_call(read<uintptr_t>(arrow+0x38),"SetZOrder",&z,4);
        }
        const auto cursor_tree=read<uintptr_t>(cursor+0x168);
        if(cursor_tree)feedback_visibility(read<uintptr_t>(cursor_tree+0x38),1);
        // Cursor-state changes toggle Enable/DisableGray independently. Their
        // tint is also suppressed on this disposable shell; the copied brush
        // on our cursor retains its original tint and texture.
        for(const char* name:{"Enable","DisableGray"}) {
            budget=64;if(const auto image=craft_find_named(cursor,name,budget);image&&feedback_is_a(image,"Image")) {
                std::array<float,4> transparent{};craft_call(image,"SetColorAndOpacity",transparent.data(),16);
            }
        }
        feedback_visibility(cursor,1);
    }
}
