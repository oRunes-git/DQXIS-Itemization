// Scoped to the expanded equipment "Show All" panel. Native setters update
// their Slate slots normally; this adapter reapplies only the requested rect.
struct ShowAllRect {float x,y,width,height;};
using CanvasLayoutFn=void(*)(void*,const void*);
using CanvasVectorFn=void(*)(void*,uint64_t);
using ScrollTextFn=void(*)(void*,void*,const void*);
ScrollTextFn original_scroll_text=nullptr;
CanvasLayoutFn original_canvas_layout=nullptr,original_canvas_offsets=nullptr;
CanvasVectorFn original_canvas_position=nullptr,original_canvas_size=nullptr;
std::map<uint64_t,ShowAllRect> show_all_rects;
std::set<std::pair<uintptr_t,int32_t>> show_all_layout_logged;
std::mutex show_all_log_mutex;
const std::array<std::pair<uintptr_t,std::array<uint8_t,5>>,5>& equipment_ui_bindings() {
    static const std::array<std::pair<uintptr_t,std::array<uint8_t,5>>,5> signatures{{
        {0x16B3310,{0x48,0x89,0x5C,0x24,0x18}},
        {0x16B4400,{0x48,0x81,0xEC,0x98,0x00}},
        {0x16B4FC0,{0x48,0x81,0xEC,0xA8,0x00}},
        {0x16B5900,{0x48,0x81,0xEC,0xA8,0x00}},
        {0x9F7E70,{0x48,0x85,0xD2,0x0F,0x84}}}};
    return signatures;
}
bool expanded_equipment_scope(uintptr_t widget) {
    bool expanded=false,equipment=false,live=false;
    for(int depth=0;widget && depth<20;++depth) {
        if(read<uint32_t>(widget+8)&(0x10U|0x20U|0x8000U|0x10000U)) return false;
        const auto cls=read<uintptr_t>(widget+0x10);
        const auto type=fname(read<uint64_t>(cls+0x18));
        if(type=="WBP_Menu_Common_ScrollContent_005_modern_C") expanded=true;
        if(type=="WBP_Menu_Soubi_Window_001_modern_C" || type=="WBP_Menu_Soubi_Window_000_modern_C") equipment=true;
        if(type=="BP_GameInstance_C") {live=true;break;}
        widget=read<uintptr_t>(widget+0x20);
    }
    return expanded && equipment && live;
}
void enforce_equipment_show_all(void* slot) noexcept {
    if(!runtime_enabled.load()) return;
    try {
        const auto address=reinterpret_cast<uintptr_t>(slot);
        const auto content=read<uintptr_t>(address+0x40);if(!content) return;
        const auto found=show_all_rects.find(read<uint64_t>(content+0x18));
        if(found==show_all_rects.end() || !expanded_equipment_scope(content)) return;
        // Reflected LayoutData is a40-byte AnchorData at+0x48. Keep all anchor
        // and alignment data intact; these targeted widgets use fixed anchors.
        std::array<float,10> layout{};
        for(size_t i=0;i<layout.size();++i) layout[i]=read<float>(address+0x48+i*4);
        if(std::any_of(layout.begin()+4,layout.end(),[](float value){return value!=0;})) return;
        const auto& rect=found->second;
        if(layout[0]==rect.x && layout[1]==rect.y && layout[2]==rect.width && layout[3]==rect.height) return;
        layout[0]=rect.x;layout[1]=rect.y;layout[2]=rect.width;layout[3]=rect.height;
        original_canvas_layout(slot,layout.data());
        std::lock_guard<std::mutex> lock(show_all_log_mutex);
        if(show_all_layout_logged.emplace(address,read<int32_t>(address+0xC)).second)
            log({{"event","equipment_show_all_layout_applied"},{"widget",fname(read<uint64_t>(content+0x18))},
                {"slot",address},{"position",Json::array({rect.x,rect.y})},{"size",Json::array({rect.width,rect.height})}});
    } catch(const std::exception& error) {log({{"event","equipment_show_all_layout_failed"},{"reason",error.what()}});}
    catch(...) {log({{"event","equipment_show_all_layout_failed"}});}
}
void canvas_layout_hook(void* slot,const void* data) {original_canvas_layout(slot,data);enforce_equipment_show_all(slot);}
void canvas_offsets_hook(void* slot,const void* data) {original_canvas_offsets(slot,data);enforce_equipment_show_all(slot);}
void canvas_position_hook(void* slot,uint64_t value) {original_canvas_position(slot,value);enforce_equipment_show_all(slot);}
void canvas_size_hook(void* slot,uint64_t value) {original_canvas_size(slot,value);enforce_equipment_show_all(slot);}
bool show_all_is_panel(uintptr_t widget) {
    auto cls=read<uintptr_t>(widget+0x10);
    for(int depth=0;cls && depth<32;++depth) {
        if(fname(read<uint64_t>(cls+0x18))=="PanelWidget") return true;
        cls=read<uintptr_t>(cls+0x40);
    }
    return false;
}
void layout_show_all_tree(uintptr_t widget,int depth,int& budget) {
    if(!widget || depth>32 || --budget<0) return;
    if(read<uint32_t>(widget+8)&(0x10U|0x20U|0x8000U|0x10000U)) return;
    const auto slot=read<uintptr_t>(widget+0x38);
    if(slot && show_all_rects.count(read<uint64_t>(widget+0x18)))
        enforce_equipment_show_all(reinterpret_cast<void*>(slot));
    if(!show_all_is_panel(widget)) return;
    // WidgetTree.AllWidgets is empty in cooked live widgets. Traverse the
    // reflected PanelWidget.Slots instead, without retaining UObject pointers.
    const auto children=read<Array>(widget+0xC8);
    if(children.count<0 || children.count>256 || children.capacity<children.count) return;
    for(int i=0;i<children.count;++i) {
        const auto child_slot=read<uintptr_t>(children.data+uintptr_t(i)*8);
        if(child_slot) layout_show_all_tree(read<uintptr_t>(child_slot+0x40),depth+1,budget);
    }
}
void scroll_text_hook(void* owner,void* data,const void* text) {
    // This native helper is called by JackUMGTouchPadScrollContent.SetText for
    // both descriptions, before native word wrapping (cached size at +0x50).
    // Patch only expanded ContentData2 in a live equipment panel; leave the
    // collapsed description and every other scroll widget untouched.
    try {
        const auto widget=reinterpret_cast<uintptr_t>(owner);
        const auto content_data=reinterpret_cast<uintptr_t>(data);
        if(runtime_enabled.load() && content_data==widget+0x260 && expanded_equipment_scope(widget)) {
            const auto tree=read<uintptr_t>(widget+0x168);
            if(tree) {int budget=256;layout_show_all_tree(read<uintptr_t>(tree+0x38),0,budget);}
            const auto content=read<uintptr_t>(content_data+0x18);
            const auto slot=content?read<uintptr_t>(content+0x38):0;
            const auto parent=slot?read<uintptr_t>(slot+0x38):0;
            const auto parent_slot=parent?read<uintptr_t>(parent+0x38):0;
            if(parent_slot && fname(read<uint64_t>(parent+0x18))=="ParentPanel2") {
                const std::array<float,2> size{read<float>(parent_slot+0x50),read<float>(parent_slot+0x54)};
                if(size[0]==656 && size[1]==414)
                    std::memcpy(reinterpret_cast<void*>(content_data+0x50),size.data(),sizeof(size));
            }
        }
    } catch(const std::exception& error) {log({{"event","equipment_show_all_layout_failed"},{"reason",error.what()}});}
    original_scroll_text(owner,data,text);
}
void create_equipment_ui_hooks() {
    for(const auto& binding:equipment_ui_bindings()) for(size_t i=0;i<binding.second.size();++i)
        if(read<uint8_t>(base+binding.first+i)!=binding.second[i])
            throw std::runtime_error("Equipment UI binding signature mismatch");
    show_all_rects.emplace(native_name("MainPanel2"),ShowAllRect{1070,80,700,530});
    show_all_rects.emplace(native_name("ParentPanel2"),ShowAllRect{22,12,656,464});
    show_all_rects.emplace(native_name("Text_Close"),ShowAllRect{524,488,160,40});
    show_all_rects.emplace(native_name("Icon_001"),ShowAllRect{490,488,36,36});
    if(MH_CreateHook(reinterpret_cast<void*>(base+0x16B3310),reinterpret_cast<void*>(&canvas_layout_hook),reinterpret_cast<void**>(&original_canvas_layout))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(base+0x16B4400),reinterpret_cast<void*>(&canvas_offsets_hook),reinterpret_cast<void**>(&original_canvas_offsets))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(base+0x16B4FC0),reinterpret_cast<void*>(&canvas_position_hook),reinterpret_cast<void**>(&original_canvas_position))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(base+0x16B5900),reinterpret_cast<void*>(&canvas_size_hook),reinterpret_cast<void**>(&original_canvas_size))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(base+0x9F7E70),reinterpret_cast<void*>(&scroll_text_hook),reinterpret_cast<void**>(&original_scroll_text))!=MH_OK)
        throw std::runtime_error("Equipment UI hook creation failed");
}
