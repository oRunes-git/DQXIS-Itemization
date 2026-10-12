#pragma once
#include "item_lifecycle.hpp"
#include <functional>

namespace itemization {
struct RarityWeights {
    int normal=0,magic=0,rare=0;
    Json json() const {return {{"normal",normal},{"magic",magic},{"rare",rare}};}
    std::string roll(Random& random) const {
        const auto n=random.below(10000);
        return n<static_cast<uint64_t>(normal)?"normal":n<static_cast<uint64_t>(normal+magic)?"magic":"rare";
    }
};
class RarityProgression {
    Json data;
    static int bp(const Json& d,const char* key) {
        require(d.at(key).is_number_integer(),std::string("Invalid rarity setting: ")+key);
        const int n=d.at(key);require(n>=0 && n<=10000,"Rarity basis points outside 0..10000");return n;
    }
public:
    explicit RarityProgression(const Json& d):data(d) {
        require(d.at("schema_version")==1,"Unsupported rarity progression");
        for(const auto k:{"initial_magic_bp","initial_rare_bp","per_boss_magic_bp","per_boss_rare_bp",
            "maximum_magic_bp","maximum_rare_bp","minimum_normal_bp"}) bp(d,k);
        require(bp(d,"maximum_magic_bp")+bp(d,"maximum_rare_bp")+bp(d,"minimum_normal_bp")<=10000,"Rarity caps exceed 100%");
        require(bp(d,"initial_magic_bp")<=bp(d,"maximum_magic_bp") && bp(d,"initial_rare_bp")<=bp(d,"maximum_rare_bp"),"Initial rarity exceeds caps");
        std::set<std::string> ids,requirements;
        for(const auto& boss:d.at("bosses")) {
            require(ids.insert(boss.at("id").get<std::string>()).second && !boss.at("requirements").empty(),"Duplicate or empty boss credit");
            for(const auto& r:boss.at("requirements")) {
                require(r.at("count").get<int>()>=1 && r.at("count").get<int>()<=100,"Invalid boss requirement count");
                require(r.contains("zukan_id") != r.contains("scenario_flag"),"Boss requirement must identify one saved record");
            }
            require(requirements.insert(boss.at("requirements").dump()).second,"Boss variants would grant duplicate credit");
        }
    }
    const Json& bosses() const {return data.at("bosses");}
    int credit_count(const std::function<int(const std::string&)>& defeats,
        const std::function<bool(const std::string&)>& flag=[](const std::string&){return false;}) const {
        int credits=0;
        for(const auto& b:bosses()) {
            bool done=true;
            for(const auto& r:b.at("requirements")) {
                if(r.contains("zukan_id")) done=done && defeats(r.at("zukan_id"))>=r.at("count").get<int>();
                else done=done && flag(r.at("scenario_flag"));
            }
            if(done) ++credits;
        }
        return credits;
    }
    RarityWeights weights(int credits) const {
        require(credits>=0,"Negative boss credit");
        const auto scale=[&](const char* initial,const char* increment,const char* cap) {
            return static_cast<int>(std::min<int64_t>(bp(data,cap),int64_t(bp(data,initial))+int64_t(credits)*bp(data,increment)));
        };
        const int magic=scale("initial_magic_bp","per_boss_magic_bp","maximum_magic_bp");
        const int rare=scale("initial_rare_bp","per_boss_rare_bp","maximum_rare_bp");
        return {10000-magic-rare,magic,rare};
    }
};
struct RegionalPool {
    std::string id,map_id,scenario;int stage=0,class_ceiling=0;
    std::set<std::string> monsters;
    struct Entry {std::string base_id,category;uint32_t weight=0;};
    std::map<std::string,std::vector<Entry>> categories;
};
class RegionalLoot {
    std::vector<RegionalPool> pools;
    std::map<std::pair<std::string,std::string>,size_t> by_map;
    std::map<std::string,uint32_t> category_weights;
    int chance=0,maximum=0;
public:
    RegionalLoot(const Json& d,const BaseRegistry& bases) {
        require(d.at("schema_version")==1 && d.at("steam_build")==6291077,"Unsupported regional loot catalog");
        chance=d.at("equipment_chance_bp");maximum=d.at("maximum_bonus_items_per_battle");
        require(chance>=0 && chance<=10000 && maximum>=1 && maximum<=25,"Invalid loot frequency");
        for(auto i=d.at("category_weights").begin();i!=d.at("category_weights").end();++i) {
            const auto w=i.value().get<int>();require(w>0 && w<=10000,"Invalid category weight");
            require(std::set<std::string>{"weapon","body","head","shield","accessory"}.count(i.key())!=0,"Unknown drop category");
            category_weights[i.key()]=static_cast<uint32_t>(w);
        }
        std::set<std::string> ids;
        for(const auto& r:d.at("regions")) {
            RegionalPool p;p.id=r.at("id");p.map_id=r.at("map_id");p.scenario=r.at("scenario");
            p.stage=r.at("acquisition_stage");p.class_ceiling=r.at("class_ceiling");
            require(ids.insert(p.id).second && p.stage>0 && p.class_ceiling>=1 && p.class_ceiling<=12,"Invalid region identity/stage");
            require(std::set<std::string>{"A","B","C","S"}.count(p.scenario)!=0,"Unknown region world state");
            for(const auto& m:r.at("monsters")) require(p.monsters.insert(m.get<std::string>()).second,"Duplicate regional monster");
            std::set<std::string> entries;
            for(const auto& i:r.at("pool")) {
                const std::string id=i.at("base_id"),category=i.at("category");
                require(bases.contains(id) && entries.insert(id).second,"Unknown or duplicate drop base");
                const auto& b=bases.get(id);
                require(b.at("category")==category && category_weights.count(category)!=0,"Drop category mismatch");
                const int c=b.at("item_class");
                require(c==i.at("item_class") && c<=p.class_ceiling && c>=std::max(1,p.class_ceiling-1),"Drop Class mismatch");
                require(b.value("automatic_acquisition",true) && std::stoi(b.at("native_fields").at("LimitedNumber").get<std::string>())==0 &&
                    std::stoi(b.at("native_fields").at("PlusValue").get<std::string>())==0,"Protected or upgraded base in regional pool");
                const int weight=i.at("weight");require(weight>0 && weight<=10000,"Invalid base drop weight");
                p.categories[category].push_back({id,category,static_cast<uint32_t>(weight)});
            }
            require(!p.categories.empty() && !p.monsters.empty(),"Empty regional drop table");
            require(by_map.emplace(std::make_pair(p.map_id,p.scenario),pools.size()).second,"Duplicate map/state pool");
            pools.push_back(std::move(p));
        }
        for(const auto& a:d.at("aliases")) {
            const std::string id=a.at("region_id");size_t index=0;
            for(;index<pools.size() && pools[index].id!=id;++index) {}
            require(index<pools.size() && pools[index].scenario==a.at("scenario"),"Invalid pool alias");
            require(by_map.emplace(std::make_pair(a.at("map_id").get<std::string>(),a.at("scenario").get<std::string>()),index).second,"Duplicate pool alias");
        }
    }
    const std::vector<RegionalPool>& regions() const {return pools;}
    int battle_limit() const {return maximum;}
    int chance_bp() const {return chance;}
    const RegionalPool* find(const std::string& map,const std::string& scenario) const {
        const auto i=by_map.find({map,scenario});return i==by_map.end()?nullptr:&pools.at(i->second);
    }
    struct EquipmentRoll {int draw,threshold;bool succeeded() const {return draw<threshold;}};
    EquipmentRoll roll_equipment(Random& random) const {return {static_cast<int>(random.below(10000)),chance};}
    bool should_drop(Random& random) const {return roll_equipment(random).succeeded();}
    std::string pick_base(const RegionalPool& p,Random& random) const {
        uint64_t total=0;for(const auto& c:p.categories) total+=category_weights.at(c.first);
        auto n=random.below(total);const std::vector<RegionalPool::Entry>* entries=nullptr;
        for(const auto& c:p.categories) {const auto w=category_weights.at(c.first);if(n<w) {entries=&c.second;break;}n-=w;}
        require(entries!=nullptr,"Empty drop categories");total=0;
        for(const auto& e:*entries) total+=e.weight;
        n=random.below(total);for(const auto& e:*entries) {if(n<e.weight)return e.base_id;n-=e.weight;}
        throw std::logic_error("Invalid base weights");
    }
};
}
