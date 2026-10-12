// Explicit one-shot test aid. XI S saves the updated count normally in game.
#define main read_only_diagnostic_main
#include "inspect.cpp"
#undef main

int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("Usage: set-test-pearls report.json");
        Reader reader;
        const std::array<std::pair<uint64_t,std::vector<uint8_t>>,4> signatures{{
            {0x928FCE,{0x4C,0x8B,0x05,0x53,0xF5,0x2D,0x05}},
            {0x928FDB,{0x41,0x8D,0x51,0x0C}},
            {0x928FE9,{0x8B,0x40,0x18}},
            {0x92343E,{0x48,0x8B,0x48,0x70,0x48,0x63,0x50,0x78,0x44,0x8B,0x40,0x58}}}};
        for (const auto& signature:signatures)
            for (size_t i=0;i<signature.second.size();++i)
                if (reader.read<uint8_t>(reader.base+signature.first+i)!=signature.second[i])
                    throw std::runtime_error("Unsupported pearl binding signature");
        const auto before=reader.snapshot();
        const auto& managers=before.at("item_managers");
        if (managers.size()!=1) throw std::runtime_error("Expected one active inventory manager");
        const auto& pearls=managers.at(0).at("perfectionist_pearls");
        if (pearls.at("item_id")!="I_USE_046" || pearls.at("entries").size()!=1)
            throw std::runtime_error("Expected one existing native pearl entry");
        const uint64_t payload=pearls.at("entries").at(0).at("payload_address");
        const int count=pearls.at("entries").at(0).at("quantity");
        const int limit=pearls.at("limit");
        if (limit!=9999 || count<0 || count>limit)
            throw std::runtime_error("Unexpected pearl count or native limit");
        if (reader.fname(payload+0x10)!="I_USE_046" || reader.read<int32_t>(payload+0x18)!=count)
            throw std::runtime_error("Pearl entry changed during inspection; retry while idle");
        HANDLE writable=OpenProcess(PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,reader.pid);
        if (!writable) throw std::runtime_error("Cannot open pearl counter for writing");
        SIZE_T written=0;
        const BOOL okay=WriteProcessMemory(writable,reinterpret_cast<void*>(payload+0x18),&limit,sizeof(limit),&written);
        CloseHandle(writable);
        if (!okay || written!=sizeof(limit)) throw std::runtime_error("Pearl write failed");
        if (reader.read<int32_t>(payload+0x18)!=limit)
            throw std::runtime_error("Pearl readback differs; inspect before retrying");
        auto after=reader.snapshot();
        auto normalized=after.at("item_managers");
        normalized.at(0).at("perfectionist_pearls").at("entries").at(0).at("quantity")=count;
        for (auto& bag:normalized.at(0).at("bags")) if (bag.at("bag")==12)
            for (auto& item:bag.at("items")) if (item.at("item_id")=="I_USE_046") item["quantity"]=count;
        const bool inventory_unchanged=normalized==before.at("item_managers");
        const bool characters_unchanged=before.at("characters")==after.at("characters");
        const bool gold_unchanged=before.at("game_players")==after.at("game_players");
        Json report{{"steam_build",6291077},{"process_id",reader.pid},{"pearls_before",count},
            {"pearls_after",limit},{"pearl_limit",limit},{"bytes_written",written},
            {"other_inventory_unchanged",inventory_unchanged},{"characters_unchanged",characters_unchanged},
            {"gold_unchanged",gold_unchanged},{"native_save_invoked",false},
            {"save_persistence","awaiting normal in-game save"},{"automatic_refill",false}};
        std::ofstream output(argv[1]);if (!output) throw std::runtime_error("Cannot create pearl report");
        output<<report.dump(2)<<'\n';output.close();
        if (!output) throw std::runtime_error("Cannot persist pearl report");
        std::cout<<report.dump(2)<<'\n';
        return inventory_unchanged && characters_unchanged && gold_unchanged ? 0 : 3;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
