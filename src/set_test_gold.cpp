// Explicit local test helper. Only the verified on-hand gold counter is written.
// Save-file persistence is performed by the game after the user saves normally.
#define main read_only_diagnostic_main
#include "inspect.cpp"
#undef main

int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("Usage: set-test-gold report.json");
        Reader reader;
        // These instructions are verified against GetGold, GetLimitOfGold and
        // SetGold on Steam build 6291077. Never write using an inferred counter.
        const std::array<std::pair<uint64_t,std::vector<uint8_t>>,3> signatures{{
            {0x7374AF,{0x8B,0x83,0xC0,0x00,0x00,0x00}},
            {0x719370,{0x8B,0x81,0xC4,0x00,0x00,0x00,0xC3}},
            {0x7443B7,{0x89,0x81,0xC0,0x00,0x00,0x00}}}};
        for (const auto& signature:signatures)
            for (size_t i=0;i<signature.second.size();++i)
                if (reader.read<uint8_t>(reader.base+signature.first+i)!=signature.second[i])
                    throw std::runtime_error("Unsupported gold binding signature");
        auto before=reader.snapshot();
        const auto& players=before.at("game_players");
        if (players.size()!=1) throw std::runtime_error("Expected one active game player");
        uint64_t player=players.at(0).at("runtime_address");
        int gold=players.at(0).at("gold"),limit=players.at(0).at("gold_limit");
        if (limit!=9999999 || gold<0 || gold>limit)
            throw std::runtime_error("Unexpected gold counter or limit");
        if (reader.read<int32_t>(player+0xC0)!=gold || reader.read<int32_t>(player+0xC4)!=limit)
            throw std::runtime_error("Gold changed during inspection; retry while idle");
        int target=limit;
        HANDLE writable=OpenProcess(PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,reader.pid);
        if (!writable) throw std::runtime_error("Cannot open game gold counter for writing");
        SIZE_T written=0;
        BOOL okay=WriteProcessMemory(writable,reinterpret_cast<void*>(player+0xC0),&target,sizeof(target),&written);
        CloseHandle(writable);
        if (!okay || written!=sizeof(target)) throw std::runtime_error("Gold write failed");
        int after_gold=reader.read<int32_t>(player+0xC0);
        if (after_gold!=target) throw std::runtime_error("Gold readback differs; inspect before retrying");
        auto after=reader.snapshot();
        Json report{{"steam_build",6291077},{"process_id",reader.pid},{"gold_before",gold},
            {"gold_after",after_gold},{"gold_limit",limit},{"bytes_written",written},
            {"native_save_invoked",false},{"save_persistence","awaiting normal in-game save"},
            {"inventory_unchanged",before.at("item_managers")==after.at("item_managers")},
            {"characters_unchanged",before.at("characters")==after.at("characters")},
            {"total_acquired_gold_unchanged",players.at(0).at("total_acquired_gold")==after.at("game_players").at(0).at("total_acquired_gold")}};
        std::ofstream output(argv[1]);if (!output) throw std::runtime_error("Cannot create gold report");
        output<<report.dump(2)<<'\n';output.close();
        if (!output) throw std::runtime_error("Cannot persist gold report");
        std::cout<<report.dump(2)<<'\n';return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
