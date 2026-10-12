#include "itemization.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    try {
        bool batch=argc==5 && std::string(argv[2])=="--bases";
        bool requests=argc==5 && std::string(argv[2])=="--requests";
        if (argc != 8 && !batch && !requests) {
            std::cerr << "Usage: affix-preview catalog.json category rarity seed instance-id base-id output.json\n"
                      << "       affix-preview catalog.json --bases bases.json output.json\n"
                      << "       affix-preview catalog.json --requests requests.json output.json\n";
            return 2;
        }
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("Cannot open catalog");
        auto catalog = itemization::Catalog::parse(itemization::Json::parse(input));
        if (requests) {
            std::ifstream requests_input(argv[3]);
            if (!requests_input) throw std::runtime_error("Cannot open roll requests");
            auto list=itemization::Json::parse(requests_input);
            if (!list.is_array()) throw std::runtime_error("Roll requests must be an array");
            itemization::Json records=itemization::Json::array();
            for (const auto& row:list) {
                itemization::Request request{row.at("instance_id"),row.at("base_id"),row.at("category"),
                    row.at("rarity"),row.at("item_level"),row.at("seed")};
                auto roll=catalog.generate(request);
                records.push_back({{"item",roll},{"details",itemization::detail_model(roll)}});
            }
            std::ofstream output(argv[4]);
            if (!output) throw std::runtime_error("Cannot open request output");
            output<<records.dump()<<'\n';output.close();
            if (!output) throw std::runtime_error("Cannot save request output");
            std::cout<<"Exported "<<records.size()<<" shared-generator rolls\n";return 0;
        }
        if (batch) {
            std::ifstream bases_input(argv[3]);
            if (!bases_input) throw std::runtime_error("Cannot open equipment base list");
            auto bases=itemization::Json::parse(bases_input);
            itemization::Json records=itemization::Json::array();
            uint64_t sequence=0;
            for (const auto& base:bases) {
                for (const std::string rarity:{"normal","magic","rare"}) {
                    itemization::Request request{"adapter-test-"+std::to_string(sequence),base.at("base_id"),
                        base.at("category"),rarity,std::max(1,base.at("item_level").get<int>()),sequence};
                    auto roll=catalog.generate(request);
                    records.push_back({{"item",roll},{"details",itemization::detail_model(roll)}});
                    ++sequence;
                }
            }
            std::ofstream output(argv[4]);
            if (!output) throw std::runtime_error("Cannot open batch output");
            output<<records.dump(2)<<'\n';output.close();
            if (!output) throw std::runtime_error("Cannot save batch output");
            std::cout<<"Exported "<<sequence<<" shared-generator rolls\n";return 0;
        }
        itemization::Request request{argv[5], argv[6], argv[2], argv[3], 1, std::stoull(argv[4])};
        auto roll = catalog.generate(request);
        auto detail = itemization::detail_model(roll);
        std::ofstream output(argv[7]);
        if (!output) throw std::runtime_error("Cannot open output");
        output << itemization::Json{{"item", roll}, {"details", detail}}.dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot save output");
        std::cout << detail.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
