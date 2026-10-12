#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;
// Read-only diagnostics for the verified worldwide Steam build (6291077).
// Offsets are from emoose/DQXIS-SDK; no injection or game functions are invoked.
class Reader {
    HANDLE process = nullptr;
    uint64_t names = 0;
    bool indirect_names = false;
    std::map<int32_t, std::string> name_cache;
    std::map<uint64_t, std::vector<std::string>> hierarchy_cache;
public:
    uint64_t base = 0;
    DWORD pid = 0;
    Reader() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate processes");
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"DRAGON QUEST XI S.exe") == 0) {
                if (pid) { CloseHandle(snapshot); throw std::runtime_error("Multiple game processes; close extra copies"); }
                pid = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        if (!pid) throw std::runtime_error("XI S is not running. Launch it in 3D mode first.");
        process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) throw std::runtime_error("Cannot open game for read-only inspection");
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate game modules");
        MODULEENTRY32W module{}; module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module)) do {
            if (_wcsicmp(module.szModule, L"DRAGON QUEST XI S.exe") == 0) base = reinterpret_cast<uint64_t>(module.modBaseAddr);
        } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
        if (!base) throw std::runtime_error("Cannot locate game module");
        if (read<uint64_t>(base + 0x36835D0) != 0x37353A38313A3132ULL)
            throw std::runtime_error("Unsupported executable signature; refusing to inspect guessed offsets");
        names = base + 0x5D7AE20;
        // Validate the actual name-pool indirection against the well-known first FName.
        try { if (name(0) == "None") return; } catch (...) {}
        name_cache.clear(); indirect_names = true;
        if (name(0) != "None") throw std::runtime_error("Name pool validation failed");
    }
    ~Reader() { if (process) CloseHandle(process); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    void bytes(uint64_t address, void* data, size_t size) const {
        if (address < 0x10000 || address > 0x00007FFFFFFFFFFFULL) throw std::runtime_error("Invalid pointer");
        SIZE_T count = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), data, size, &count) || count != size)
            throw std::runtime_error("Game memory unavailable; retry while game is idle");
    }
    template<class T> T read(uint64_t address) const { T value{}; bytes(address, &value, sizeof(value)); return value; }
    std::string name(int32_t index) {
        if (index < 0 || index >= 4*1024*1024) throw std::runtime_error("Invalid FName index");
        auto found = name_cache.find(index); if (found != name_cache.end()) return found->second;
        uint64_t pool = indirect_names ? read<uint64_t>(names) : names;
        auto chunk = read<uint64_t>(pool + uint64_t(index / 16384)*8);
        auto pointer = read<uint64_t>(chunk + uint64_t(index % 16384)*8);
        auto encoded_index = read<uint32_t>(pointer);
        std::string value;
        // Read one code unit at a time; don't cross a committed region just to find a terminator.
        if (encoded_index & 1) {
            std::wstring wide;
            for (int i=0; i<1024; ++i) { auto c=read<wchar_t>(pointer+16+i*2); if (!c) break; wide.push_back(c); }
            int length=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
            value.resize(length); if (length) WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),value.data(),length,nullptr,nullptr);
        } else {
            for (int i=0; i<1024; ++i) { auto c=read<char>(pointer+16+i); if (!c) break; value.push_back(c); }
        }
        if (value.empty()) throw std::runtime_error("Empty FName");
        name_cache.emplace(index,value); return value;
    }
    std::string fname(uint64_t address) {
        auto result=name(read<int32_t>(address)); auto number=read<int32_t>(address+4);
        if (number > 0) result += "_" + std::to_string(number-1);
        return result;
    }
    std::string object_name(uint64_t object) { return fname(object+0x18); }
    std::string string_value(uint64_t address) {
        auto data=read<uint64_t>(address);auto count=read<int32_t>(address+8);
        if (count==0) return {};
        if (count<1 || count>8192) throw std::runtime_error("Invalid diagnostic string");
        std::wstring wide(count,0);bytes(data,wide.data(),count*sizeof(wchar_t));
        if (wide.back()!=0) throw std::runtime_error("Unterminated diagnostic string");
        auto length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),count-1,nullptr,0,nullptr,nullptr);
        std::string result(length,0);
        if (length) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),count-1,result.data(),length,nullptr,nullptr);
        return result;
    }
    Json text_candidates(uint64_t address) const {
        // Read-only FText diagnostics. ITextData layout is not yet bound; report
        // plausible display/source FString values as candidates, never as proof
        // that a widget rendered them or that its layout has sufficient space.
        std::set<std::string> candidates;
        auto data=read<uint64_t>(address);
        auto try_string=[&](uint64_t location) {
            try {
                auto pointer=read<uint64_t>(location);
                auto count=read<int32_t>(location+8),capacity=read<int32_t>(location+12);
                if (count<2 || count>1024 || capacity<count || capacity>4096) return;
                std::vector<wchar_t> wide(count);bytes(pointer,wide.data(),wide.size()*2);
                if (wide.back()!=0) return;
                for (int i=0;i<count-1;++i) if (!wide[i] || (wide[i]<32 && wide[i]!=10 && wide[i]!=13 && wide[i]!=9)) return;
                int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),count-1,nullptr,0,nullptr,nullptr);
                if (length<=0) return;
                std::string value(length,0);
                WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),count-1,value.data(),length,nullptr,nullptr);
                candidates.insert(std::move(value));
            } catch (const std::exception&) {}
        };
        for (uint64_t offset=0;offset<0x60;offset+=8) {
            try_string(data+offset);
            try {try_string(read<uint64_t>(data+offset));} catch (const std::exception&) {}
        }
        return Json(candidates);
    }
    std::string object_path(uint64_t object) {
        std::string result=object_name(object);
        auto outer=read<uint64_t>(object+0x20);
        for (int depth=0; outer && depth<12; ++depth) {
            result=object_name(outer)+"."+result; outer=read<uint64_t>(outer+0x20);
        }
        return result;
    }
    const std::vector<std::string>& hierarchy(uint64_t cls) {
        const uint64_t key=cls;
        auto found=hierarchy_cache.find(cls);
        if (found!=hierarchy_cache.end()) return found->second;
        std::vector<std::string> chain;
        for (int depth=0; cls && depth<32; ++depth) {
            chain.push_back(object_name(cls));
            cls=read<uint64_t>(cls+0x40); // UField(0x40), then UStruct::SuperField
        }
        return hierarchy_cache.emplace(key,std::move(chain)).first->second;
    }
    Json snapshot() {
        auto object_array=read<uint64_t>(base+0x5D83BF8+0x10);
        auto maximum=read<int32_t>(base+0x5D83BF8+0x18);
        auto count=read<int32_t>(base+0x5D83BF8+0x1C);
        if (count <= 0 || count > maximum || maximum > 4000000) throw std::runtime_error("Object array not ready or layout unsupported");
        struct ObjectEntry { uint64_t object; uint32_t flags; int32_t cluster; int32_t serial; int32_t padding; };
        static_assert(sizeof(ObjectEntry)==24);
        std::vector<ObjectEntry> objects(count); bytes(object_array,objects.data(),objects.size()*sizeof(ObjectEntry));
        Json result{{"schema_version",1},{"steam_build",6291077},{"read_only",true},{"process_id",pid},
            {"object_count",count},{"equipment_ui_entries",Json::array()},{"characters",Json::array()},
            {"item_tables",Json::array()},{"item_managers",Json::array()},{"save_item_candidates",Json::array()},
            {"save_objects",Json::array()},{"integration_functions",Json::array()},{"item_properties",Json::array()},
            {"item_details",Json::array()},{"equipment_list_entries",Json::array()},
            {"loaded_test_names",Json::array()},{"game_players",Json::array()},
            {"forge_accessors",Json::array()},{"message_controllers",Json::array()},{"common_accessors",Json::array()},
            {"character_parameter_properties",Json::array()},{"ui_widgets",Json::array()},
            {"ui_layout_properties",Json::array()},{"ui_layout_functions",Json::array()},
            {"ui_sound_sets",Json::array()},
            {"ui_primitives",Json::array()},
            {"skipped_unavailable_objects",0}};
        auto pool=indirect_names?read<uint64_t>(names):names;
        // This build has 128 chunk pointers (2M capacity), not the SDK's 256.
        auto name_count=read<int32_t>(pool+128*8);
        auto name_chunks=read<int32_t>(pool+128*8+4);
        result["name_count"]=name_count;
        if (name_count>0 && name_count<2*1024*1024 && name_chunks==(name_count+16383)/16384) {
            for (int index=0;index<name_count;++index) {
                try {
                    auto chunk=read<uint64_t>(pool+uint64_t(index/16384)*8);
                    auto pointer=read<uint64_t>(chunk+uint64_t(index%16384)*8);
                    if (read<uint32_t>(pointer)&1) continue;
                    std::array<char,32> text{};bytes(pointer+16,text.data(),text.size());
                    text.back()=0;
                    std::string candidate(text.data());
                    if (candidate.find("ARPG_TEST")!=std::string::npos || candidate.rfind("ARPG_I_",0)==0)
                        result["loaded_test_names"].push_back(candidate);
                } catch (const std::exception&) {}
            }
        }
        std::map<uint64_t,std::string> class_names;
        int skipped=0;
        for (const auto& entry : objects) {
            if (!entry.object || (entry.flags & ((1U<<28)|(1U<<29)))) continue;
            try {
                auto flags=read<uint32_t>(entry.object+8);
                if(object_name(entry.object)=="Default__WidgetBlueprintLibrary")result["crafting_library"]={{"object",object_path(entry.object)},{"flags",flags},{"runtime_address",entry.object}};
                if (flags & (0x10U|0x20U|0x8000U|0x10000U)) continue; // defaults, archetypes, destroyed objects
                auto cls=read<uint64_t>(entry.object+0x10);
                auto found=class_names.find(cls);
                std::string type;
                if (found==class_names.end()) { type=object_name(cls); class_names[cls]=type; } else type=found->second;
                const auto& chain=hierarchy(cls);
                auto is_a=[&](const std::string& wanted) { return std::find(chain.begin(),chain.end(),wanted)!=chain.end(); };
                if(is_a("JackUMGSoundSet")) {
                    const auto cue=read<uint64_t>(entry.object+0x58);
                    result["ui_sound_sets"].push_back({{"object",object_path(entry.object)},{"basic_enter",cue?object_path(cue):"None"},{"cue_class",cue?object_name(read<uint64_t>(cue+0x10)):"None"}});
                }
                if(is_a("JackUMGManager")) {
                    Json focus{{"manager",object_path(entry.object)},{"address",entry.object}};
                    for(const auto& field:std::map<std::string,int>{{"page",0x3D0},{"exception_page",0x3E8},{"menu",0x278}}) {
                        const auto value=read<uint64_t>(entry.object+field.second);
                        focus[field.first]={{"address",value},{"object",value?object_path(value):"None"}};
                        if(value && field.first!="menu") {const auto widget=read<uint64_t>(value+0x38);focus[field.first]["widget"]=widget?object_path(widget):"None";focus[field.first]["widget_address"]=widget;}
                    }
                    result["crafting_focus"].push_back(std::move(focus));
                }
                if(type=="JackUMGManager" || type=="WBP_Menu_Main_Command_000_modern_C" || type=="BP_GameInstance_C") {
                    result["crafting_object_headers"].push_back({{"object",object_path(entry.object)},{"class",type},{"runtime_address",entry.object},
                        {"index",read<int32_t>(entry.object+0xC)},{"flags",flags},{"class_address",cls},{"class_index",read<int32_t>(cls+0xC)},
                        {"outer",read<uint64_t>(entry.object+0x20)},
                        {"masked_class_slot",read<uint64_t>(read<uint64_t>(base+0x5D83BF8+0x10)+uint64_t(read<uint32_t>(cls+0xC)&0x7fffffffU)*24)}});
                    if(type=="BP_GameInstance_C") result["crafting_object_headers"].back()["item_manager"]={{"address",read<uint64_t>(entry.object+0x130)},{"object",object_path(read<uint64_t>(entry.object+0x130))}};
                }
                if(is_a("Class") && object_name(entry.object)=="Image" && object_path(entry.object)=="/Script/UMG.Image") {
                    const auto defaults=read<uint64_t>(entry.object+0x108);
                    if(defaults) result["ui_primitives"].push_back({{"class",object_path(entry.object)},
                        {"default_draw_as",read<uint8_t>(defaults+0x134)},
                        {"default_resource",read<uint64_t>(defaults+0x110)}});
                }
                if (is_a("Widget") && object_path(entry.object).rfind("/Engine/Transient.",0)==0) {
                    Json widget{{"object",object_path(entry.object)},{"class",type},{"runtime_address",entry.object},
                        {"visibility",read<uint8_t>(entry.object+0x86)}};
                    if(is_a("JackUMGWidgetBase")) {
                        widget["native_visible"]=read<uint8_t>(entry.object+0x1F8);
                        widget["native_position"]={read<float>(entry.object+0x248),read<float>(entry.object+0x24C)};
                        widget["native_root_position"]={read<float>(entry.object+0x264),read<float>(entry.object+0x268)};
                        widget["native_size"]={read<float>(entry.object+0x25C),read<float>(entry.object+0x260)};
                        widget["skip_local_position"]=read<uint8_t>(entry.object+0x204);
                    }
                    if(is_a("JackUMGItemBase")){
                        widget["selectable"]=read<uint8_t>(entry.object+0x370);
                        widget["item_id"]=fname(entry.object+0x380);
                        widget["data_number"]=read<int32_t>(entry.object+0x3BC);
                    }
                    if(type=="WBP_Menu_Dougu_Command_000_modern_C"){
                        widget["selectable_items"]=Json::array();widget["cursor_links"]=Json::array();
                        const auto data=read<uint64_t>(entry.object+0x380);const auto size=read<int32_t>(entry.object+0x388);
                        if(size>=0&&size<=32)for(int i=0;i<size;++i){const auto row=read<uint64_t>(data+uint64_t(i)*8);widget["selectable_items"].push_back(row?object_path(row):"None");}
                        const auto moves=read<uint64_t>(entry.object+0x3F0);const auto move_count=read<int32_t>(entry.object+0x3F8);
                        if(move_count>=0&&move_count<=128)for(int i=0;i<move_count;++i){const auto setting=moves+uint64_t(i)*24;const auto from=read<uint64_t>(setting),to=read<uint64_t>(setting+16);widget["cursor_links"].push_back({{"from",from?object_path(from):"None"},{"direction",read<uint8_t>(setting+8)},{"to",to?object_path(to):"None"}});}
                    }
                    if(is_a("JackUMGWidgetBase") && type=="WBP_Menu_Main_Command_000_modern_C") {
                        widget["input_flags"]=Json::array();for(int offset=0x1F8;offset<=0x210;++offset) widget["input_flags"].push_back(read<uint8_t>(entry.object+offset));
                        widget["window_in_focus"]=read<uint8_t>(entry.object+0x378);
                    }
                    if(type=="WBP_Menu_Main_Command_000_modern_C" || type=="WBP_UMG_Root_Widget_modern_C" || type.find("WBP_Menu_Soubi_")==0 || type.find("WBP_Menu_Dougu_")==0 || type=="WBP_Menu_Parts_008_modern_C" || type.find("WBP_Menu_Common_Setumei_")==0 || type.find("FusigiKazi")!=std::string::npos || is_a("JackUMGItemBase")) {
                        widget["fields"]=Json::array();
                        for(auto c=cls;c;c=read<uint64_t>(c+0x40)) for(auto f=read<uint64_t>(c+0x48);f;f=read<uint64_t>(f+0x38)) {
                            const auto ft=object_name(read<uint64_t>(f+0x10));
                            if(ft=="ObjectProperty") {
                                const auto offset=read<int32_t>(f+0x54);const auto value=read<uint64_t>(entry.object+offset);
                                widget["fields"].push_back({{"name",object_name(f)},{"offset",offset},{"value",value},{"object",value?object_path(value):"None"}});
                            }
                        }
                    }
                    Json transform=Json::array();for(int i=0;i<7;++i) transform.push_back(read<float>(entry.object+0x60+uint64_t(i)*4));
                    widget["render_transform"]=transform;
                    if(type=="WBP_UMG_FusigiKazi_Game_Base_000_modern_C") {
                        widget["reflected_field_chain"]=Json::array();
                        auto field=read<uint64_t>(cls+0x48);
                        for(int budget=256;field && budget>0;--budget,field=read<uint64_t>(field+0x38))
                            widget["reflected_field_chain"].push_back(object_name(field));
                    }
                    if(type=="WBP_Hud_FusigiKazi_Gauge_modern_C") {
                        widget["gauge_fields"]=Json::object();
                        for(const auto& field:std::map<std::string,int>{{"hp",0x430},{"lower",0x434},{"upper",0x438},{"maximum",0x43C},{"success_lower",0x440},{"success_upper",0x444},{"image_width",0x44C},{"ideal_lower",0x464},{"ideal_upper",0x468},{"gauge_width",0x47C},{"default_width",0x488}})
                            widget["gauge_fields"][field.first]=read<float>(entry.object+field.second);
                        widget["reverse"]=read<uint8_t>(entry.object+0x484);
                    }
                    if(is_a("TextBlock")) {
                        const auto font_object=read<uint64_t>(entry.object+0x158);
                        widget["font_object"]=font_object?object_path(font_object):"None";
                        widget["font_bytes"]=Json::array();
                        for(int i=0;i<0x58;++i) widget["font_bytes"].push_back(read<uint8_t>(entry.object+0x158+i));
                        widget["font_size"]=read<int32_t>(entry.object+0x1A8);
                        widget["justification"]=read<uint8_t>(entry.object+0xD0);
                        widget["text_border_width"]=read<float>(entry.object+0x200);
                        widget["text_character_spacing"]=read<float>(entry.object+0x204);
                        widget["text_border_colour"]=Json::array();widget["text_shadow_colour"]=Json::array();
                        for(int i=0;i<4;++i) {
                            widget["text_border_colour"].push_back(read<float>(entry.object+0x1E0+i*4));
                            widget["text_shadow_colour"].push_back(read<float>(entry.object+0x1B8+i*4));
                        }
                        widget["text_shadow_offset"]=Json::array({read<float>(entry.object+0x1B0),read<float>(entry.object+0x1B4)});
                        widget["wrap_at"]=read<float>(entry.object+0xD4);
                        widget["auto_wrap"]=read<uint8_t>(entry.object+0xD1);
                        widget["fixed_line_height"]=read<float>(entry.object+0xF0);
                        if(object_path(entry.object).find("ScrollContent_005")!=std::string::npos || object_path(entry.object).find("FusigiKazi_Game")!=std::string::npos || object_path(entry.object).find("WBP_Menu_Message_")!=std::string::npos || object_path(entry.object).find("WBP_Menu_Parts_000")!=std::string::npos)
                            widget["text_candidates"]=text_candidates(entry.object+0xF8);
                    }
                    if(is_a("Image") && (object_path(entry.object).find("FusigiKazi_Game_Base")!=std::string::npos || object_path(entry.object).find("Soubi_")!=std::string::npos || object_path(entry.object).find("Dougu_")!=std::string::npos || object_path(entry.object).find("Common_Setumei_")!=std::string::npos || object_path(entry.object).find("UMG_Root_Widget")!=std::string::npos)) {
                        const auto resource=read<uint64_t>(entry.object+0x110);
                        widget["brush_resource"]=resource?object_path(resource):"None";
                        widget["brush_draw_as"]=read<uint8_t>(entry.object+0x134);
                        widget["brush_margin"]=Json::array();for(int i=0;i<4;++i)widget["brush_margin"].push_back(read<float>(entry.object+0xD8+i*4));
                        widget["brush_tint"]=Json::array();widget["image_tint"]=Json::array();
                        for(int i=0;i<4;++i) {
                            widget["brush_tint"].push_back(read<float>(entry.object+0xE8+i*4));
                            widget["image_tint"].push_back(read<float>(entry.object+0x150+i*4));
                        }
                    }
                    if(is_a("JackUMGTouchPadScrollContent")) {
                        widget["scroll_content_data"]=Json::array();
                        for(auto offset:{0x1D8,0x260}) {
                            const auto data=entry.object+offset;
                            widget["scroll_content_data"].push_back({{"offset",offset},{"initialized",read<uint8_t>(data)},
                                {"parent_size",Json::array({read<float>(data+0x50),read<float>(data+0x54)})},
                                {"content_size",Json::array({read<float>(data+0x40),read<float>(data+0x44)})},
                                {"tag_mode",read<uint8_t>(data+0x78)},{"line_height",read<float>(data+0x7C)},
                                {"text_blocks",read<int32_t>(data+0x28)}});
                        }
                    }
                    const auto slot=read<uint64_t>(entry.object+0x38);
                    if(slot) {
                        widget["slot"]=object_path(slot);widget["slot_address"]=slot;
                        const auto parent=read<uint64_t>(slot+0x38);
                        if(parent) widget["parent"]=object_path(parent);
                        if(object_name(read<uint64_t>(slot+0x10))=="CanvasPanelSlot") {
                            Json layout=Json::array();for(int i=0;i<10;++i) layout.push_back(read<float>(slot+0x48+uint64_t(i)*4));
                            widget["canvas_layout"]=layout;widget["canvas_auto_size"]=read<uint8_t>(slot+0x70);
                        }
                    }
                    result["ui_widgets"].push_back(std::move(widget));
                }
                if (is_a("JackUMGItemSyousai")) {
                    const auto id=fname(entry.object+0x48);
                    if (id.rfind("W_",0)!=0 && id.rfind("E_",0)!=0 && id.rfind("A_",0)!=0 && id.rfind("ARPG_I_",0)!=0) continue;
                    Json detail{{"object",object_path(entry.object)}, {"base_id",id},
                        {"bag",read<uint8_t>(entry.object+0x39)}, {"index_in_bag",read<int32_t>(entry.object+0x3C)},
                        {"quantity",read<int32_t>(entry.object+0x80)}, {"classification",read<uint8_t>(entry.object+0xA9)}};
                    if (id.find("ARPG_TEST")!=std::string::npos || id.rfind("ARPG_I_",0)==0) {
                        detail["name_text_candidates"]=text_candidates(entry.object+0x50);
                        detail["description_text_candidates"]=text_candidates(entry.object+0xD0);
                    }
                    result["item_details"].push_back(std::move(detail));
                } else if (is_a("JackUMGItemData")) {
                    const auto id=fname(entry.object+0x40);
                    if (id.rfind("W_",0)!=0 && id.rfind("E_",0)!=0 && id.rfind("A_",0)!=0 && id.rfind("ARPG_I_",0)!=0) continue;
                    Json item{{"object",object_path(entry.object)}, {"base_id",id},
                        {"data_number",read<int32_t>(entry.object+0x38)}, {"general_value",read<int32_t>(entry.object+0x50)}};
                    if (id.find("ARPG_TEST")!=std::string::npos || id.rfind("ARPG_I_",0)==0) {
                        item["text_id"]=fname(entry.object+0x48);
                        auto data=read<uint64_t>(entry.object+0x58);
                        auto size=read<int32_t>(entry.object+0x60),capacity=read<int32_t>(entry.object+0x64);
                        if (size>=0 && size<=128 && capacity>=size && capacity<=128) {
                            Json containers=Json::array();
                            for (int index=0;index<size;++index) {
                                auto container=read<uint64_t>(data+uint64_t(index)*8);
                                containers.push_back({{"tag",fname(container+0x40)},
                                    {"type",read<uint8_t>(container+0x38)}, {"value",read<int32_t>(container+0x48)},
                                    {"text_candidates",text_candidates(container+0x50)}});
                            }
                            item["containers"]=std::move(containers);
                        }
                    }
                    result["equipment_list_entries"].push_back(std::move(item));
                } else if (is_a("JackUMGItemBagInfo")) {
                    if (!read<uint8_t>(entry.object+0xB0)) continue;
                    result["equipment_ui_entries"].push_back({{"object",object_path(entry.object)},
                        {"base_id",fname(entry.object+0x40)}, {"bag",read<uint8_t>(entry.object+0x38)},
                        {"index_in_bag",read<int32_t>(entry.object+0x3C)}, {"quantity",read<int32_t>(entry.object+0xD8)},
                        {"classification",read<int32_t>(entry.object+0x90)}});
                } else if (is_a("JackUMGAccessorFusigiKazi")) {
                    result["forge_accessors"].push_back({{"object",object_path(entry.object)},
                        {"runtime_address",entry.object},{"create_category",read<uint8_t>(entry.object+0x40)},
                        {"parts_type",read<uint8_t>(entry.object+0x41)},
                        {"game_state",read<uint8_t>(entry.object+0x44)},
                        {"result",read<int32_t>(entry.object+0x41C)},
                        {"retry_flag",read<uint8_t>(entry.object+0x420)},
                        {"game_base_widget",read<uint64_t>(entry.object+0x110)},
                        {"gauges",Json::array()},
                        {"retry_base_quality",read<int32_t>(entry.object+0x428)},
                        {"retry_bag",read<int32_t>(entry.object+0x42C)},
                        {"retry_base_item_id",fname(entry.object+0x430)}});
                    for(int i=0;i<8;++i) {
                        const auto gauge=entry.object+0x24C+i*0x28;
                        Json values=Json::array();for(int f=0;f<10;++f) values.push_back(read<float>(gauge+f*4));
                        result["forge_accessors"].back()["gauges"].push_back(values);
                    }
                } else if (is_a("JackUMGRetryItemSyousai")) {
                    result["item_details"].push_back({{"object",object_path(entry.object)},
                        {"type","forge_rework"},{"base_id",fname(entry.object+0x40)},
                        {"name_text_candidates",text_candidates(entry.object+0x48)},
                        {"quantity",read<int32_t>(entry.object+0x60)},
                        {"equipped",read<uint8_t>(entry.object+0x64)},
                        {"recipe_name",fname(entry.object+0x68)},
                        {"index_in_bag",read<int32_t>(entry.object+0x70)}});
                } else if (is_a("JackUMGSerifuWindowLatentMessageEndResult")) {
                    result["message_controllers"].push_back({{"object",object_path(entry.object)},
                        {"kind","latent_result"},{"sender",read<uint64_t>(entry.object+0x38)},
                        {"text_id",fname(entry.object+0x40)},{"selected_index",read<int32_t>(entry.object+0x48)}});
                } else if (is_a("JackUMGSerifuWindowController")) {
                    auto previous_index=read<int32_t>(entry.object+0x418);auto previous_serial=read<int32_t>(entry.object+0x41C);
                    std::string previous_focus;
                    if(previous_index>=0 && previous_index<count && objects[previous_index].serial==previous_serial && objects[previous_index].object)
                        previous_focus=object_path(objects[previous_index].object);
                    Json pages=Json::array();auto page_data=read<uint64_t>(entry.object+0x290);auto page_count=read<int32_t>(entry.object+0x298);
                    if(page_count>=0 && page_count<=64) for(int i=0;i<page_count;++i) {
                        auto page=page_data+uint64_t(i)*0x100;
                        pages.push_back({{"text_id",fname(page)},{"flags_cc_to_cf",Json::array({read<uint8_t>(page+0xCC),read<uint8_t>(page+0xCD),read<uint8_t>(page+0xCE),read<uint8_t>(page+0xCF)})}});
                    }
                    auto args=read<uint64_t>(entry.object+0x438);auto length=read<int32_t>(entry.object+0x440);
                    if (length<0 || length>128) continue;
                    Json values=Json::array();
                    for (int index=0;index<length;++index) {
                        auto argument=args+uint64_t(index)*0x48;
                        values.push_back({{"name",string_value(argument)},{"value",string_value(argument+0x10)},
                            {"id",fname(argument+0x20)},{"table_type",read<uint8_t>(argument+0x28)},
                            {"prior_replace",read<uint8_t>(argument+0x41)},{"list",read<uint8_t>(argument+0x42)}});
                    }
                    result["message_controllers"].push_back({{"object",object_path(entry.object)},
                        {"runtime_address",entry.object},{"current_text_id",fname(entry.object+0x180)},
                        {"native_state",read<int32_t>(entry.object+0x1F0)},{"next_state",read<int32_t>(entry.object+0x1F4)},
                        {"page_count",read<int32_t>(entry.object+0x298)},{"current_page",read<int32_t>(entry.object+0x2F8)},
                        {"select_window",read<uint64_t>(entry.object+0x278)},{"latent_result",read<uint64_t>(entry.object+0x580)},
                        {"previous_focus_index",read<int32_t>(entry.object+0x418)},
                        {"previous_focus_serial",read<int32_t>(entry.object+0x41C)},
                        {"previous_focus_object",previous_focus},{"parsed_pages",pages},
                        {"state_handlers",Json::array({read<uint64_t>(entry.object+0x200)-base,read<uint64_t>(entry.object+0x208)-base,
                            read<uint64_t>(entry.object+0x210)-base,read<uint64_t>(entry.object+0x218)-base,
                            read<uint64_t>(entry.object+0x220)-base,read<uint64_t>(entry.object+0x228)-base,
                            read<uint64_t>(entry.object+0x230)-base,read<uint64_t>(entry.object+0x238)-base,
                            read<uint64_t>(entry.object+0x240)-base,read<uint64_t>(entry.object+0x248)-base})},
                        {"selected_index",read<int32_t>(entry.object+0x344)},{"arguments",std::move(values)}});
                } else if (is_a("JackUMGManager")) {
                    auto focus_page=read<uint64_t>(entry.object+0x3E8);if(!focus_page) focus_page=read<uint64_t>(entry.object+0x3D0);
                    auto focus_widget=focus_page?read<uint64_t>(focus_page+0x38):0;
                    result["common_accessors"].push_back({{"object",object_path(entry.object)},{"kind","ui_manager"},
                        {"focused_widget",focus_widget?object_path(focus_widget):"None"},{"focus_page",focus_page}});
                } else if (is_a("JackUMGAccessorCommon")) {
                    result["common_accessors"].push_back({{"object",object_path(entry.object)},
                        {"runtime_address",entry.object}});
                } else if (is_a("JackGamePlayer")) {
                    // Verified native GetGold / GetLimitOfGold / SetGold bindings.
                    result["game_players"].push_back({{"object",object_path(entry.object)},
                        {"runtime_address",entry.object},{"gold",read<int32_t>(entry.object+0xC0)},
                        {"gold_limit",read<int32_t>(entry.object+0xC4)},
                        {"total_acquired_gold",read<int32_t>(entry.object+0xC8)}});
                } else if (is_a("JackFriendGameCharacter")) {
                    auto vtable=read<uint64_t>(entry.object);
                    Json character{{"object",object_path(entry.object)}, {"character_id",fname(entry.object+0x38)},
                        {"character_type",read<uint8_t>(entry.object+0x75)}, {"level",read<int32_t>(entry.object+0x78)},
                        {"hp",read<int32_t>(entry.object+0x7C)}, {"max_hp",read<int32_t>(entry.object+0xA0)},
                        {"equipment_getter_rva",read<uint64_t>(vtable+0x2D8)-base}};
                    Json innate=Json::object();
                    for (auto [stat,offset]:std::array<std::pair<const char*,uint64_t>,9>{{
                        {"strength",0x84},{"resilience",0x88},{"agility",0x8C},
                        {"deftness",0x90},{"charm",0x94},{"magical_might",0x98},
                        {"magical_mending",0x9C},{"max_hp",0xA0},{"max_mp",0xA4}}})
                        innate[stat]=read<int32_t>(entry.object+offset);
                    character["stored_parameters"]=innate;
                    Json candidate_arrays=Json::array();
                    for (uint64_t offset=0xD8; offset+16<=0x650; offset+=8) {
                        try {
                            auto pointer=read<uint64_t>(entry.object+offset);
                            auto size=read<int32_t>(entry.object+offset+8), capacity=read<int32_t>(entry.object+offset+12);
                            if (size<=0 || size>6 || capacity<size || capacity>32) continue;
                            Json equipped=Json::array();
                            for (int i=0;i<size;++i) {
                                auto slot=pointer+uint64_t(i)*16; auto id=fname(slot);
                                if (id.rfind("W_",0)!=0 && id.rfind("E_",0)!=0 && id.rfind("A_",0)!=0 && id.rfind("ARPG_I_",0)!=0) { equipped=Json::array(); break; }
                                equipped.push_back({{"base_id",id},{"region",read<uint8_t>(slot+8)}});
                            }
                            if (!equipped.empty()) candidate_arrays.push_back({{"offset",offset},{"entries",equipped}});
                        } catch (const std::exception&) {}
                    }
                    character["candidate_equipment_arrays"]=candidate_arrays;
                    result["characters"].push_back(character);
                } else if (is_a("DataTable")) {
                    auto row=read<uint64_t>(entry.object+0x38);
                    if (!row) continue;
                    auto row_type=object_name(row);
                    if(row_type=="JackDataTableGameFlag") {
                        Json claims=Json::array();
                        const auto data=read<uint64_t>(entry.object+0x40);
                        const int count=read<int32_t>(entry.object+0x48);
                        if(count>0 && count<=20000 && data) for(int i=0;i<count;++i) try {
                            const auto slot=data+uint64_t(i)*24;
                            const auto id=fname(slot);
                            if(id.rfind("ARPG_VEYL_",0)!=0)continue;
                            const auto value=read<uint64_t>(slot+8);
                            claims.push_back({{"id",id},{"flag_no",fname(value+0x10)},
                                {"flags",read<uint8_t>(value+0x30)},{"read_only",bool(read<uint8_t>(value+0x30)&1)}});
                        } catch(const std::exception&) {}
                        result["veyl_claim_tables"].push_back({{"object",object_path(entry.object)},{"claims",claims}});
                    }
                    if (row_type.find("Item")!=std::string::npos || row_type.find("Equipment")!=std::string::npos) {
                        Json table{{"object",object_path(entry.object)}, {"row_type",row_type}};
                        if (row_type=="JackDataTableItem") {
                            std::array<uint8_t,0x50> header{}; bytes(entry.object+0x40,header.data(),header.size());
                            std::vector<uint64_t> words;
                            for (size_t offset=0; offset<header.size(); offset+=8) {
                                uint64_t value; std::memcpy(&value,header.data()+offset,8); words.push_back(value);
                            }
                            table["rowmap_words"]=words;
                            auto data=read<uint64_t>(entry.object+0x40);
                            auto elements=read<int32_t>(entry.object+0x48);
                            auto capacity=read<int32_t>(entry.object+0x4C);
                            table["rowmap_num"]=elements; table["rowmap_capacity"]=capacity;
                            if (elements>0 && elements<=20000 && capacity>=elements && capacity<=20000) {
                                Json actual_rows=Json::array();
                                for (int i=0; i<elements; ++i) {
                                    try {
                                        auto slot=data+uint64_t(i)*24;
                                        auto id=fname(slot); auto value=read<uint64_t>(slot+8);
                                        if (id.rfind("I_SHARD_",0)==0 || id.rfind("I_VEYL_",0)==0) {
                                            if(!table.contains("currency_rows"))table["currency_rows"]=Json::array();
                                            table["currency_rows"].push_back({{"id",id},{"native_key",read<uint64_t>(slot)},
                                                {"name_index",read<int32_t>(slot)},{"name_number",read<int32_t>(slot+4)},
                                                {"pool_name",name(read<int32_t>(slot))},{"classification",fname(value+0x28)},
                                                {"store_common_bag",read<uint8_t>(value+0x115)},{"limited_number",read<int32_t>(value+0x118)}});
                                        }
                                        if (id.rfind("W_",0)!=0 && id.rfind("E_",0)!=0 && id.rfind("A_",0)!=0) continue;
                                        actual_rows.push_back({{"base_id",id},{"classification",fname(value+0x28)},
                                            {"attack",read<int32_t>(value+0x98)}, {"defence",read<int32_t>(value+0x9C)},
                                            {"agility",read<int32_t>(value+0xC4)}, {"charm",read<int32_t>(value+0xD8)}});
                                    } catch (const std::exception&) {}
                                }
                                table["equipment_rows"]=actual_rows;
                            }
                        }
                        result["item_tables"].push_back(table);
                    }
                } else if (is_a("JackItemManager")) {
                    Json manager{{"object",object_path(entry.object)},
                        {"runtime_address",entry.object},
                        {"item_table",object_path(read<uint64_t>(entry.object+0x38))}, {"bags",Json::array()}};
                    // Native GetUtinaosiNoHouju (0x928FC0) selects bag 12 and
                    // the FName at 0x5C08528; GetLimitUtinaosiNoHouju (0x923430)
                    // uses the manager's default +0x58 or its per-item override.
                    const auto pearl_name=read<uint64_t>(base+0x5C08528);
                    Json pearls{{"item_id",fname(base+0x5C08528)},
                        {"default_limit",read<int32_t>(entry.object+0x58)},
                        {"limit",read<int32_t>(entry.object+0x58)},
                        {"entries",Json::array()}};
                    auto limit_data=read<uint64_t>(entry.object+0x70);
                    auto limit_count=read<int32_t>(entry.object+0x78);
                    if (limit_count<0 || limit_count>10000) throw std::runtime_error("Unsupported item limit array");
                    for (int i=0;i<limit_count;++i) {
                        const auto slot=limit_data+uint64_t(i)*16;
                        if (read<uint64_t>(slot)==pearl_name)
                            pearls["limit"]=read<int32_t>(slot+8);
                    }
                    // StoreItemToBag (Steam RVA 0x7692A0) selects 0x30-byte bags at
                    // +0x128 and walks 16-byte TSharedPtr entries. Item payloads
                    // store FName at +0x10 and quantity at +0x18. These runtime
                    // pointers describe the current inventory, not persistent IDs.
                    for (int bag=0;bag<=12;++bag) {
                        auto header=entry.object+0x128+uint64_t(bag)*0x30;
                        auto data=read<uint64_t>(header);
                        auto size=read<int32_t>(header+8), capacity=read<int32_t>(header+12);
                        if (size<0 || size>10000 || capacity<size || capacity>10000) continue;
                        Json items=Json::array();
                        for (int index=0;index<size;++index) {
                            try {
                                auto item=read<uint64_t>(data+uint64_t(index)*16);
                                auto id=fname(item+0x10);
                                auto quantity=read<int32_t>(item+0x18);
                                if (bag==12 && read<uint64_t>(item+0x10)==pearl_name)
                                    pearls["entries"].push_back({{"payload_address",item},
                                        {"quantity",quantity},{"index",index}});
                                if (id=="None" || quantity<=0) continue;
                                items.push_back({{"item_id",id},{"quantity",quantity},{"index",index}});
                            } catch (const std::exception&) {}
                        }
                        if (!items.empty()) manager["bags"].push_back({{"bag",bag},{"items",items}});
                    }
                    manager["perfectionist_pearls"]=std::move(pearls);
                    result["item_managers"].push_back(std::move(manager));
                } else if (is_a("JackSaveData")) {
                    result["save_objects"].push_back({{"object",object_path(entry.object)}, {"class",type},
                        {"current_map",fname(entry.object+0x318)}});
                    std::array<uint8_t,0xC040> save{}; bytes(entry.object,save.data(),save.size());
                    for (size_t offset=0x38; offset+8<=save.size(); offset+=4) {
                        int32_t index=0,number=0;
                        std::memcpy(&index,save.data()+offset,4); std::memcpy(&number,save.data()+offset+4,4);
                        if (index<=0 || index>=4*1024*1024 || number!=0) continue;
                        try {
                            auto candidate=name(index);
                            if (candidate.rfind("W_",0)==0 || candidate.rfind("E_",0)==0)
                                result["save_item_candidates"].push_back({{"object",object_path(entry.object)},
                                    {"offset",offset},{"possible_item_id",candidate}});
                        } catch (const std::exception&) {}
                    }
                } else if (is_a("Function")) {
                    const auto function_name=object_name(entry.object);
                    const auto path=object_path(entry.object);
                    if(path.rfind("/Script/UMG.",0)==0 || path.find("WBP_Menu_Dougu_")!=std::string::npos || path.find("WBP_Menu_Soubi_")!=std::string::npos || path.find("WBP_Menu_Common_Setumei_")!=std::string::npos || path.find("JackUMGItemBase.")!=std::string::npos || path.find("JackUMGBrowserBase.")!=std::string::npos || path.find("JackUMGAccessorItem.")!=std::string::npos || path.find("JackUMGAccessorMembers.")!=std::string::npos || path.find("JackUMGManager.")!=std::string::npos || path.find("JackUMGWidgetBase.")!=std::string::npos || path.find("JackUMGWindowBase.")!=std::string::npos || path.find("Main_Command")!=std::string::npos || path.find("JackUIInput")!=std::string::npos || path.find("FusigiKazi")!=std::string::npos || path.find("KismetTextLibrary.Conv_StringToText")!=std::string::npos || path.find("GameplayStatics.SpawnObject")!=std::string::npos || path.find("JackUMGText.")!=std::string::npos || path.find("JackUMGTouchPadScrollContent.")!=std::string::npos)
                        result["ui_layout_functions"].push_back({{"object",path},{"runtime_address",entry.object},
                            {"relative_native_function",read<uint64_t>(entry.object+0xC0)-base},
                            {"parameter_size",read<uint16_t>(entry.object+0x9E)}});
                    const std::set<std::string> targets{"Gold","GetGold","IncreaseGold","GetLimitOfGold","DecreaseGold","ActionItem","ActionBulkBuy","GetActionItemId","GetShopCurrency","StoreItem","GetItemCount","GetItemCountInBag","GetPossessionItemCount","GetItemGettableCount","IsExistItem","GetTotalAcquisitionItemCount","GetEquippedItems","GetEquippedParameters","GetEquippedParametersWith", "GetItemSyousaiFromBagTypeAndItemIndex","GetItemParameter","GetDescription","GetItemName","GetMainSlotIndex","SaveGame","LoadGame"};
                    const std::set<std::string> message_targets{"MakeItemGetMessageTag","MakeItemGetMessageTextID","GetItemNameFromId","GetListItemNameFromId","GetTextFromTextId","DisplaySerifuWindowAtTextID","DispMessageWithTextId","DispMenuMessageWithTextId","MacroReplaceBP","ReplaceMessageTag","GetSelectionContent","GetTextParseResult","SetTextAtTextID","InitTextAtTextID","SetTextAtFixedMessage","DoSelectSimpleDelegate","OnSelectWindowControl","AddChoicesContentByFixedText","GetTextRaw"};
                    if (targets.count(function_name) || message_targets.count(function_name) || function_name.find("Houju")!=std::string::npos ||
                        object_path(entry.object).find("JackUMGAccessorFusigiKazi.")!=std::string::npos ||
                        object_path(entry.object).find("JackUMGSerifuWindowController.")!=std::string::npos) {
                        result["integration_functions"].push_back({{"object",object_path(entry.object)},
                            {"relative_native_function",read<uint64_t>(entry.object+0xC0)-base},
                            {"parameter_size",read<uint16_t>(entry.object+0x9E)},
                            {"return_offset",read<uint16_t>(entry.object+0xA0)}});
                    }
                } else if (is_a("Property")) {
                    const auto outer=read<uint64_t>(entry.object+0x20);
                    if(outer) {
                        const auto owner=object_path(outer);
                        if(owner.rfind("/Script/UMG.",0)==0 || owner.find("WBP_Menu_Dougu_")!=std::string::npos || owner.find("WBP_Menu_Soubi_")!=std::string::npos || owner.find("WBP_Menu_Common_Setumei_")!=std::string::npos || owner.find("JackUMGAccessorItem")!=std::string::npos || owner.find("JackUMGWindowBase")!=std::string::npos || owner.find("JackUMGItemSyousai")!=std::string::npos || owner.find("JackUMGItemBase")!=std::string::npos || owner.find("JackUMGItemData")!=std::string::npos || owner.find("JackUMGSoundSet")!=std::string::npos || owner.find("FusigiKazi")!=std::string::npos || owner.find("JackUMGText")!=std::string::npos || owner.find("JackUMGTouchPadScrollContent")!=std::string::npos)
                            result["ui_layout_properties"].push_back({{"owner",owner},{"name",object_name(entry.object)},
                                {"class",type},{"element_size",read<int32_t>(entry.object+0x44)},
                                {"offset",read<int32_t>(entry.object+0x54)}});
                    }
                    if (outer && object_name(outer).find("Parameter")!=std::string::npos &&
                        (object_name(outer).find("Character")!=std::string::npos || object_name(outer).find("Chara")!=std::string::npos)) {
                        result["character_parameter_properties"].push_back({{"owner",object_path(outer)},
                            {"name",object_name(entry.object)},{"class",type},
                            {"element_size",read<int32_t>(entry.object+0x44)},
                            {"offset",read<int32_t>(entry.object+0x54)}});
                    }
                    if (outer && object_name(outer)=="JackDataTableItem") {
                        const auto property_name=object_name(entry.object);
                        result["item_properties"].push_back({{"name",property_name},{"class",type},
                            {"element_size",read<int32_t>(entry.object+0x44)},
                            {"offset",read<int32_t>(entry.object+0x54)}});
                    }
                }
            } catch (const std::exception&) { ++skipped; }
        }
        result["skipped_unavailable_objects"]=skipped;
        return result;
    }
};

int main(int argc,char** argv) {
    try {
        if (argc != 2) { std::cerr << "Usage: inspect-game output.json\n"; return 2; }
        Reader reader; auto snapshot=reader.snapshot();
        const std::filesystem::path destination(argv[1]);
        std::ofstream output(destination); if (!output) throw std::runtime_error("Cannot open diagnostic output");
        output << snapshot.dump(2) << '\n'; output.close();
        if (!output) throw std::runtime_error("Cannot save diagnostic output");
        std::cout << "Read-only snapshot saved: " << destination.string() << "\n"
            << "Equipment entries: " << snapshot["equipment_ui_entries"].size() << ", item tables: "
            << snapshot["item_tables"].size() << ", characters: " << snapshot["characters"].size() << "\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
