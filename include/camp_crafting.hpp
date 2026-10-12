#pragma once
#include "item_description.hpp"
#include <optional>
namespace itemization {
enum class CampMode {Veyl,Shard,Quality,Salvage,MassSalvage};
inline const char* camp_mode_name(CampMode m) {
    switch(m) {case CampMode::Veyl:return "Veyl crafting";case CampMode::Shard:return "Shard crafting";
    case CampMode::Quality:return "Reforge Quality";case CampMode::Salvage:return "Salvage gear";default:return "Mass salvage";}
}
class CampCraftPolicy {
    Json data;
    std::map<std::string,Json> families;
public:
    explicit CampCraftPolicy(Json config):data(std::move(config)) {
        require(data.at("schema_version")==2,"Unsupported campsite crafting policy");
        const int first=data.at("salvage_base_chance_bp"),bonus=data.at("salvage_bonus_chance_bp"),limit=data.at("mass_salvage_limit");
        require(first>=0 && first<=10000 && bonus>=0 && bonus<=10000 && first+bonus>0 && limit>=1 && limit<=100,"Invalid salvage policy");
        std::set<std::string> native_ids;
        for(const auto& s:data.at("shards")) {
            require(families.emplace(s.at("family_id"),s).second && native_ids.insert(s.at("native_id")).second,"Duplicate shard identity");
            require(s.at("native_id").get<std::string>().find("I_SHARD_")==0,"Invalid native shard identity");
        }
        const auto weights=data.at("tier_weights").get<std::vector<uint32_t>>();require(weights.size()==maximum_affix_tier,"Expected thirty-three shard tier weights");
        for(auto w:weights) require(w>0 && w<=1000000,"Invalid shard tier weight");
    }
    const Json& shards() const {return data.at("shards");}
    const Json& definition(const std::string& family) const {return families.at(family);}
    int maximum_items() const {return data.at("mass_salvage_limit");}
    int base_chance() const {return data.at("salvage_base_chance_bp");}
    int bonus_chance() const {return data.at("salvage_bonus_chance_bp");}
    int shard_chance(const ItemService& service,const Json& effect) const {
        if(!families.count(effect.at("id"))) return 0; // Retired families yield no invalid currency.
        const int rank=service.affix_tier(effect).at("rank");
        require(rank>=1 && rank<=maximum_affix_tier,"Invalid salvaged modifier tier");
        return base_chance();
    }
    bool protected_base(const Json& item) const {
        const auto& base=item.at("base_snapshot");const auto& fields=base.at("native_fields");
        return !base.value("automatic_acquisition",true) || std::stoi(fields.value("LimitedNumber",std::string("0")))>0 ||
            fields.value("SellType",std::string("EJackItem_SellType__OK"))!="EJackItem_SellType__OK";
    }
    bool protected_item(const Json& item) const {return item.value("locked",false)||protected_base(item);}
    bool protected_item(const ItemService& service,const Json& item) const {
        return service.is_locked(item)||protected_base(item);
    }
    Json salvage_preview(const ItemService& service,const std::vector<Json>& items) const {
        require(!items.empty() && items.size()<=static_cast<size_t>(maximum_items()),"Invalid salvage selection");
        Json result=Json::array();std::set<std::string> ids;
        for(const auto& item:items) {
            validate_instance(item);require(ids.insert(item.at("engine_id")).second,"Duplicate salvage item");
            if(service.is_locked(item)) continue; // Stale selections never roll shards for locked gear.
            require(!protected_item(service,item),"Protected salvage item");
            for(const auto& a:item.at("affixes")) {
                if(!families.count(a.at("id")))continue;
                const int first=shard_chance(service,a),rank=service.affix_tier(a).at("rank");
                result.push_back({{"family_id",a.at("id")},{"item",instance_name(item)},
                    {"label",definition(a.at("id")).at("name")},{"rank",rank},{"base_chance_bp",first},
                    {"bonus_chance_bp",bonus_chance()},{"bonus_rolls",rank-1},
                    {"maximum_shards",rank},{"expected_shards_bp",first+(rank-1)*bonus_chance()}});
            }
        }
        return result;
    }
    template<class RandomSource> Json salvage_roll(const ItemService& service,const std::vector<Json>& items,RandomSource& random) const {
        Json drops=Json::object();
        for(const auto& p:salvage_preview(service,items)) {
            const std::string id=p.at("family_id");
            int count=random.below(10000)<p.at("base_chance_bp").get<uint64_t>()?1:0;
            for(int i=0;i<p.at("bonus_rolls").get<int>();++i)
                if(random.below(10000)<p.at("bonus_chance_bp").get<uint64_t>())++count;
            if(count)drops[id]=drops.value(id,0)+count;
        }
        return drops;
    }
    Json apply(const ItemService& service,const Json& item,const std::string& family,uint64_t seed) const {
        definition(family);return service.craft_shard(item.at("engine_id"),family,seed,data.at("tier_weights").get<std::vector<uint32_t>>());
    }
    bool eligible(const ItemService& service,const Json& item,const std::string& family) const {
        try {apply(service,item,family,0);return true;} catch(const std::exception&) {return false;}
    }
    std::string craft_description(const ItemService& service,const Json& item,const std::string& family) const {
        const auto& name=definition(family).at("label");
        for(const auto& a:item.at("affixes"))if(a.at("id")==family) {
            const int rank=service.affix_tier(a).at("rank");
            if(!eligible(service,item,family))return "Cannot upgrade "+name.get<std::string>()+" further on this item. Class, equipment type and tier limits apply.";
            return "Upgrade "+name.get<std::string>()+": T"+std::to_string(rank)+" -> T"+std::to_string(service.next_upgrade_rank(item,a))+
                ".\nRolls a value in the next tier. Item Class limits apply.";
        }
        return "Adds "+name.get<std::string>()+" to an empty eligible slot.\nRandom tier and value; item Class limits apply.";
    }
};
struct CampRow {std::string key,label;bool available=true;};
struct CampSelection {bool finished=false,apply=false;std::string shard;std::vector<std::string> salvage;};
class CampCraftMenu {
    const ItemService& service;const CampCraftPolicy& policy;
    CampMode mode;Json item,wallet;std::vector<Json> gear;
    enum class Stage {List,Confirm,Preview,Result,Finished};Stage stage=Stage::List;
    size_t page=0,preview_page=0;std::string family,result;
    std::set<std::string> selected;
    std::vector<std::string> owned_shards;
    std::vector<Json> selected_items() const {
        std::vector<Json> items;for(const auto& g:gear) if(selected.count(g.at("engine_id"))) items.push_back(g);return items;
    }
public:
    CampCraftMenu(const ItemService& s,const CampCraftPolicy& p,CampMode m,Json target,Json balances,std::vector<Json> inventory={}):
        service(s),policy(p),mode(m),item(std::move(target)),wallet(std::move(balances)),gear(std::move(inventory)) {
        require(m==CampMode::Shard || m==CampMode::Salvage || m==CampMode::MassSalvage,"Unsupported custom campsite menu");
        for(const auto& sh:p.shards()) if(wallet.value(sh.at("family_id").get<std::string>(),0)>0) owned_shards.push_back(sh.at("family_id"));
        std::sort(owned_shards.begin(),owned_shards.end(),[&](const auto& a,const auto& b) {return p.definition(a).at("name")<p.definition(b).at("name");});
        if(m==CampMode::Salvage) {gear={item};selected.insert(item.at("engine_id"));stage=Stage::Confirm;}
    }
    std::string stage_name() const {
        switch(stage) {case Stage::List:return "list";case Stage::Confirm:return "confirm";case Stage::Preview:return "preview";case Stage::Result:return "result";default:return "finished";}
    }
    std::vector<CampRow> rows() const {
        std::vector<CampRow> rows;
        if(stage==Stage::Result) return {{"done","Return to equipment",true}};
        if(stage==Stage::Finished) return {};
        if(stage==Stage::List) {
            const size_t total=mode==CampMode::Shard?owned_shards.size():gear.size();
            for(size_t i=page*6;i<std::min(total,page*6+6);++i) {
                if(mode==CampMode::Shard) {
                    const auto& id=owned_shards[i];const auto& d=policy.definition(id);
                    const bool legal=policy.eligible(service,item,id);
                    rows.push_back({id,d.at("name").get<std::string>()+" x"+std::to_string(wallet.at(id).get<int>())+(legal?"":" (ineligible)"),legal});
                } else {const std::string id=gear[i].at("engine_id");rows.push_back({id,std::string(selected.count(id)?"[x] ":"[ ] ")+instance_name(gear[i]),selected.count(id) || selected.size()<static_cast<size_t>(policy.maximum_items())});}
            }
            if((page+1)*6<total) rows.push_back({"next","Next page",true});
            if(page) rows.push_back({"previous","Previous page",true});
            if(mode==CampMode::MassSalvage) rows.push_back({"review","Review "+std::to_string(selected.size())+" selected",!selected.empty()});
            rows.push_back({"cancel","Cancel",true});return rows;
        }
        if(stage==Stage::Preview) {
            const auto preview=policy.salvage_preview(service,selected_items());
            if((preview_page+1)*6<preview.size()) rows.push_back({"next_preview","Next chances",true});
            if(preview_page) rows.push_back({"previous_preview","Previous chances",true});
            rows.push_back({"back","Back",true});rows.push_back({"cancel","Cancel",true});return rows;
        }
        rows.push_back({"apply",mode==CampMode::Shard?"Spend 1 shard":"Salvage permanently",true});
        if(mode!=CampMode::Shard) rows.push_back({"preview","View shard chances",true});
        rows.push_back({"back","Back",true});rows.push_back({"cancel","Cancel",true});return rows;
    }
    std::string text() const {
        std::string text=camp_mode_name(mode);text+="\r\n";
        if(stage==Stage::Result) text+=result;
        else if(stage==Stage::Preview) {
            const auto preview=policy.salvage_preview(service,selected_items());
            if(preview.empty()) text+="These items have no recoverable modifiers.\r\n";
            for(size_t i=preview_page*6;i<std::min(preview.size(),preview_page*6+6);++i) text+=preview[i].at("label").get<std::string>()+" (T"+
                std::to_string(preview[i].at("rank").get<int>())+"): 1 x "+display_number(policy.base_chance()/100.0)+"% + "+preview[i].at("bonus_rolls").dump()+" x "+display_number(policy.bonus_chance()/100.0)+"%\r\n";
        } else if(mode==CampMode::Shard) {
            text+=instance_name(item)+"\r\n";
            if(stage==Stage::Confirm) text+=policy.craft_description(service,item,family)+"\r\n";
            else text+=owned_shards.empty()?"No shards owned. Salvage spare gear to find shards.\r\n":"Choose a shard to fill an empty slot or upgrade its matching modifier.\r\n";
        } else {
            text+="Selected: "+std::to_string(selected.size())+" equipment items.\r\n";
            if(stage==Stage::Confirm) {
                const auto preview=policy.salvage_preview(service,selected_items());double expected=0;for(const auto& p:preview) expected+=p.at("expected_shards_bp").get<int>()/10000.0;
                text+="Permanently consumes the selection. Shards are not guaranteed.\r\n";
                text+="Expected shards: "+display_number(std::round(expected*100)/100)+". View chances before confirming.\r\n";
            } else text+="Choose items, then review. Equipped/protected gear is excluded.\r\n";
        }
        text+="<select>\r\n";for(const auto& r:rows()) text+=r.label+"\r\n";return text+"<select_end><end>";
    }
    CampSelection select(int index) {
        const auto choices=rows();require(index>=0 && index<static_cast<int>(choices.size()),"Invalid campsite selection");
        const auto& r=choices[index];if(!r.available) return {};
        if(r.key=="cancel" || r.key=="done") {stage=Stage::Finished;return {true,false,{},{}};}
        if(r.key=="next") ++page;
        else if(r.key=="previous") --page;
        else if(r.key=="next_preview") ++preview_page;
        else if(r.key=="previous_preview") --preview_page;
        else if(r.key=="review") {stage=Stage::Confirm;preview_page=0;}
        else if(r.key=="preview") {stage=Stage::Preview;preview_page=0;}
        else if(r.key=="back") {
            if(stage==Stage::Preview) stage=Stage::Confirm;
            else if(mode==CampMode::Salvage) {stage=Stage::Finished;return {true,false,{},{}};}
            else stage=Stage::List;
        } else if(r.key=="apply") {
            stage=Stage::Result;CampSelection selection;selection.apply=true;selection.shard=family;
            selection.salvage={selected.begin(),selected.end()};return selection;
        } else if(mode==CampMode::Shard) {family=r.key;stage=Stage::Confirm;}
        else if(selected.count(r.key)) selected.erase(r.key);
        else {require(selected.size()<static_cast<size_t>(policy.maximum_items()),"Mass salvage limit reached");selected.insert(r.key);}
        return {};
    }
    void show_result(std::string message) {result=std::move(message);stage=Stage::Result;}
};
}
