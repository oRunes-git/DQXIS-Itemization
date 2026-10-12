// Included after runtime_text. The macro renderer deep-copies its const argument
// array before resolving it. Borrowed strings here remain alive for that call;
// their storage must never be passed to an engine destructor or allocator.
using MacroReplaceFn=Array*(*)(void*,Array*,const Array*,const Array*,bool);
MacroReplaceFn original_macro_replace=nullptr;
struct MessageArgumentView {
    Array name,value;
    uint64_t id;
    uint8_t table_type;
    std::array<uint8_t,0x18> grammar;
    uint8_t prior_replace,list;
    std::array<uint8_t,5> padding;
};
static_assert(sizeof(MessageArgumentView)==0x48);
static_assert(offsetof(MessageArgumentView,table_type)==0x28);
static_assert(offsetof(MessageArgumentView,prior_replace)==0x41);
Array* macro_replace_hook(void* manager,Array* output,const Array* text,const Array* arguments,bool list) {
    std::vector<MessageArgumentView> borrowed;
    std::vector<std::wstring> labels;
    Array view{};bool changed=false;
    try {
        if (runtime_enabled.load() && arguments) {
            const auto input=read<Array>(reinterpret_cast<uintptr_t>(arguments));
            if (input.count<0 || input.count>1024 || input.capacity<input.count)
                throw std::runtime_error("Invalid message argument array");
            borrowed.resize(input.count);labels.resize(input.count);
            std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
            for (int i=0;i<input.count;++i) {
                borrowed[i]=read<MessageArgumentView>(input.data+uintptr_t(i)*sizeof(MessageArgumentView));
                auto found=runtime_ids.find(borrowed[i].id);
                if (borrowed[i].table_type!=2 || found==runtime_ids.end()) continue;
                labels[i]=wide_utf8(itemization::instance_name(item_service->lookup(found->second)));
                borrowed[i].value={reinterpret_cast<uintptr_t>(labels[i].c_str()),
                    static_cast<int32_t>(labels[i].size()+1),static_cast<int32_t>(labels[i].size()+1)};
                borrowed[i].id=0;borrowed[i].table_type=0; // Literal text, like the native CategoryName argument.
                changed=true;
                static std::map<std::string,bool> logged;
                if (logged.emplace(found->second,true).second)
                    log({{"event","runtime_message_name_resolved"},{"engine_id",found->second},
                        {"name",itemization::instance_name(item_service->lookup(found->second))}});
            }
            view={reinterpret_cast<uintptr_t>(borrowed.data()),input.count,input.count};
        }
    } catch (const std::exception& error) {
        changed=false;log({{"event","runtime_message_name_failed"},{"reason",error.what()}});
    }
    // Call once, outside the preparation catch: a partially rendered output must
    // not be rendered a second time. Original caller-owned arguments stay intact.
    return original_macro_replace(manager,output,text,changed?&view:arguments,list);
}
