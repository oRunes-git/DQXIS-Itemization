#pragma once
#include "item_lifecycle.hpp"
#include "item_display.hpp"
#include <optional>

namespace itemization {
inline std::string veyl_short_name(const std::string& name) {
    if(name.rfind("Veyl of ",0)==0) return name.substr(8);
    if(name.size()>=5 && name.compare(name.size()-5,5," Veyl")==0) return name.substr(0,name.size()-5);
    return name;
}
struct VeylChoice {std::string currency,target;};
class VeylPolicy {
    Json data;
    std::map<std::string,Json> definitions;
public:
    explicit VeylPolicy(const Json& d):data(d) {
        require(d.at("schema_version")==1,"Unsupported Veyl policy");
        const std::set<std::string> expected{"shifting","waking","binding","blooming","tempest","severing","tuning","surging","kingmaker"};
        std::set<std::string> ids;
        for(const auto& c:d.at("currencies")) {
            const std::string id=c.at("id"),native=c.at("native_id");
            require(expected.count(id) && definitions.emplace(id,c).second && ids.insert(native).second,"Duplicate or unknown Veyl");
            require(native.find("I_VEYL_")==0 && c.at("name").get<std::string>().find("Veyl")!=std::string::npos,"Invalid Veyl identity");
            const int cost=c.at("cost");require(cost>=1 && cost<=9999,"Invalid Veyl cost");
            const int rare_cost=c.at("rare_cost");require(rare_cost>=1 && rare_cost<=9999,"Invalid rare-item Veyl cost");
            const auto tiers=c.at("tier_costs").get<std::vector<int>>();require(tiers.size()==maximum_affix_tier,"Expected thirty-three Veyl tier costs");
            for(auto n:tiers) require(n>=1 && n<=9999,"Invalid Veyl tier cost");
        }
        require(definitions.size()==expected.size(),"Expected nine Veyls");
        const auto& kingmaker=definitions.at("kingmaker");
        require(kingmaker.at("acquisition_flag")=="ARPG_VEYL_KINGMAKER_ACQUIRED" && kingmaker.at("cost")==1 && kingmaker.at("hidden_until_acquired")==true,"Invalid Kingmaker acquisition policy");
        const auto weights=d.at("craft_tier_weights").get<std::vector<uint32_t>>();require(weights.size()==maximum_affix_tier,"Expected thirty-three crafting weights");
        for(auto w:weights) require(w>0 && w<=1000000,"Invalid crafting weight");
        for(const auto& p:d.at("drop_pools")) {
            for(const auto field:{"initial_bp","per_boss_bp","maximum_bp"}) {
                require(p.at(field).is_number_integer(),"Invalid Veyl drop chance");const int n=p.at(field);require(n>=0 && n<=10000,"Veyl drop rate out of range");
            }
            require(p.at("initial_bp").get<int>()<=p.at("maximum_bp").get<int>(),"Initial Veyl rate exceeds cap");
            const int cls=p.at("minimum_class");require(cls>=1 && cls<=12,"Invalid Veyl drop Class");
            int total=0;for(auto w=p.at("weights").begin();w!=p.at("weights").end();++w) {
                const int n=w.value();require(definitions.count(w.key()) && n>0 && n<=1000000,"Invalid Veyl drop weight");total+=n;
            }
            require(total>0,"Empty Veyl drop pool");
        }
        for(const auto& list:{d.at("boss_bundle"),d.at("story_bundle"),d.at("late_boss_bonus")})
            for(auto q=list.begin();q!=list.end();++q) require(definitions.count(q.key()) && q.value().is_number_integer() && q.value().get<int>()>=1 && q.value().get<int>()<=9999,"Invalid Veyl reward bundle");
        // Its late-game award location is intentionally not configured yet.
        for(const auto& p:d.at("drop_pools")) require(!p.at("weights").contains("kingmaker"),"Kingmaker must not drop randomly");
        for(const auto& list:{d.at("boss_bundle"),d.at("story_bundle"),d.at("late_boss_bonus")}) require(!list.contains("kingmaker"),"Kingmaker award location is not configured");
        const int late=d.at("late_boss_minimum_class");require(late>=1 && late<=12,"Invalid late-boss Class");
        std::set<std::string> encounters,claims;claims.insert(d.at("migration_flag"));
        for(const auto& e:d.at("encounters")) {
            require(encounters.insert(e.at("event_id")).second && claims.insert(e.at("claim_flag")).second,"Duplicate story Veyl encounter/claim");
            require(e.at("kind")=="boss" || e.at("kind")=="story","Unknown Veyl encounter kind");
        }
    }
    const Json& config() const {return data;}
    const Json& currencies() const {return data.at("currencies");}
    const Json& definition(const std::string& id) const {return definitions.at(id);}
    bool visible(const std::string& id,const Json& wallet) const {
        return !definition(id).value("hidden_until_acquired",false) || wallet.value(id,0)>0 || wallet.value("kingmaker_acquired",false);
    }
    bool targeted(const std::string& id) const {return id=="tuning" || id=="surging";}
    int cost(const ItemService& service,const Json& item,const VeylChoice& choice) const {
        const auto& c=definition(choice.currency);
        if(choice.currency=="surging") {
            for(const auto& a:item.at("affixes")) if(a.at("id")==choice.target) {
                const int next=service.next_upgrade_rank(item,a);
                require(next>=1 && next<=maximum_affix_tier,"No next modifier tier");return c.at("tier_costs").at(next-1);
            }
            throw std::invalid_argument("Missing Surging target");
        }
        if(choice.currency=="binding" && item.at("rarity")=="rare") return c.at("rare_cost");
        return c.at("cost");
    }
    Json apply(const ItemService& service,const Json& item,const VeylChoice& choice,uint64_t seed) const {
        definition(choice.currency);
        require(targeted(choice.currency)!=choice.target.empty(),"Invalid Veyl target selection");
        return service.currency_edit(item.at("engine_id"),choice.currency,choice.target,seed,data.at("craft_tier_weights").get<std::vector<uint32_t>>());
    }
    bool eligible(const ItemService& service,const Json& item,const VeylChoice& choice) const {
        try {
            definition(choice.currency);cost(service,item,choice);
            require(targeted(choice.currency)!=choice.target.empty(),"Invalid Veyl target selection");
            if(targeted(choice.currency)) return service.can_craft(item,choice.target,choice.currency=="tuning"?"reroll_value":"upgrade_tier",100);
            if(choice.currency=="severing") return !item.at("affixes").empty();
            if(choice.currency=="shifting") return item.at("rarity")=="normal" || item.at("rarity")=="magic";
            if(choice.currency=="tempest") return item.at("rarity")=="rare";
            apply(service,item,choice,0);return true;
        }catch(const std::exception&) {return false;}
    }
    int rate(const Json& pool,int credits,int region_class) const {
        require(credits>=0 && region_class>=1 && region_class<=12,"Invalid Veyl progression");
        if(region_class<pool.at("minimum_class").get<int>()) return 0;
        return static_cast<int>(std::min<int64_t>(pool.at("maximum_bp").get<int>(),
            int64_t(pool.at("initial_bp").get<int>())+int64_t(credits)*pool.at("per_boss_bp").get<int>()));
    }
    Json roll_drops(Random& random,int credits,int region_class) const {
        Json drops=Json::object();
        for(const auto& p:data.at("drop_pools")) {
            if(random.below(10000)>=static_cast<uint64_t>(rate(p,credits,region_class))) continue;
            uint64_t total=0;for(auto w=p.at("weights").begin();w!=p.at("weights").end();++w) total+=w.value().get<uint32_t>();
            auto n=random.below(total);for(auto w=p.at("weights").begin();w!=p.at("weights").end();++w) {
                const auto v=w.value().get<uint32_t>();if(n<v) {drops[w.key()]=drops.value(w.key(),0)+1;break;}n-=v;
            }
        }
        return drops;
    }
    Json encounter_bundle(bool boss,int region_class) const {
        Json bundle=data.at(boss?"boss_bundle":"story_bundle");
        if(boss && region_class>=data.at("late_boss_minimum_class").get<int>())
            for(auto q=data.at("late_boss_bonus").begin();q!=data.at("late_boss_bonus").end();++q) bundle[q.key()]=bundle.value(q.key(),0)+q.value().get<int>();
        return bundle;
    }
};

class VeylMenu {
public:
    struct Row {std::string label,key;bool available=true;};
    struct Selection {bool finished=false;std::optional<VeylChoice> choice;int cost=0;};
private:
    enum class Page {Currency,Target,Confirm,Result,Finished};
    Page page=Page::Currency;
    Json item,options=Json::array(),wallet;
    const VeylPolicy& policy;
    std::string name,result;
    VeylChoice choice;
    size_t offset=0;
    const Json* selected() const {
        for(const auto& o:options) if(o.at("currency")==choice.currency && o.at("target")==choice.target) return &o;
        return nullptr;
    }
public:
    VeylMenu(const ItemService& service,const VeylPolicy& p,Json source,Json balances):item(std::move(source)),wallet(std::move(balances)),policy(p),name(instance_name(item)) {
        for(auto& a:item["affixes"]) a["tier"]=service.affix_tier(a);
        for(const auto& c:policy.currencies()) {
            const std::string id=c.at("id");
            std::vector<std::string> targets;
            if(policy.targeted(id)) for(const auto& a:item.at("affixes")) targets.push_back(a.at("id"));else targets.push_back("");
            for(const auto& target:targets) {
                VeylChoice ch{id,target};if(!policy.eligible(service,item,ch)) continue;
                const int price=policy.cost(service,item,ch);
                options.push_back({{"currency",id},{"target",target},{"cost",price},{"affordable",wallet.value(id,0)>=price}});
            }
        }
    }
    std::string stage_name() const {return page==Page::Currency?"veyl":page==Page::Target?"modifier":page==Page::Confirm?"confirm":page==Page::Result?"result":"finished";}
    std::vector<Row> rows() const {
        std::vector<Row> out;
        if(page==Page::Currency) {
            Json cs=Json::array();for(const auto& c:policy.currencies())if(policy.visible(c.at("id"),wallet))cs.push_back(c);
            const auto end=std::min(cs.size(),offset+6);
            for(size_t i=offset;i<end;++i) {
                const auto& c=cs[i];const std::string id=c.at("id");bool legal=false,affordable=false;
                for(const auto& o:options) if(o.at("currency")==id) {legal=true;affordable=affordable || o.at("affordable").get<bool>();}
                out.push_back({c.at("name").get<std::string>()+" x"+std::to_string(wallet.value(id,0))+(legal?(affordable?"":" - insufficient"):" - unavailable"),id,legal&&affordable});
            }
            if(end<cs.size()) out.push_back({"Next page","next"});if(offset) out.push_back({"Previous page","previous"});
        } else if(page==Page::Target) {
            for(const auto& a:item.at("affixes")) {
                const std::string id=a.at("id");bool okay=false;
                for(const auto& o:options) if(o.at("currency")==choice.currency && o.at("target")==id) okay=o.at("affordable");
                const auto& tier=a.at("tier");
                out.push_back({std::string(a.at("kind")=="prefix"?"Prefix: ":"Suffix: ")+display_stat_label(a.at("label"))+" +"+a.at("value").dump()+
                    (a.at("unit")=="percentage_points"?"%":"")+" [T"+tier.at("rank").dump()+"]"+(okay?"":" - unavailable"),id,okay});
            }
        } else if(page==Page::Confirm) out.push_back({"Apply Veyl","apply"});
        else if(page==Page::Result) return {{"Return to equipment","done"}};
        else return out;
        if(page!=Page::Currency) out.push_back({"Back","back"});out.push_back({"Cancel","cancel"});return out;
    }
    std::string text() const {
        std::string body=name+"\r\n";
        if(page==Page::Result) body=result+"\r\n";
        else if(page==Page::Confirm) {
            const auto* o=selected();require(o,"Veyl confirmation has no selected action");
            body+=policy.definition(choice.currency).at("description").get<std::string>()+"\r\nCost: "+o->at("cost").dump()+" "+policy.definition(choice.currency).at("name").get<std::string>()+".\r\n";
            if(!choice.target.empty()) for(const auto& a:item.at("affixes")) if(a.at("id")==choice.target)
                body+=display_stat_label(a.at("label"))+" +"+a.at("value").dump()+(a.at("unit")=="percentage_points"?"%":"")+" [T"+a.at("tier").at("rank").dump()+"]\r\n";
        } else body+="Use Veyls to reshape equipment.\r\nChanges apply instantly.\r\n";
        body+="<select>";const auto list=rows();for(size_t i=0;i<list.size();++i) {if(i)body+="\r\n";body+=list[i].label;}return body+="</select><end>";
    }
    void show_result(const std::string& message) {result=message;page=Page::Result;}
    Selection select(int index) {
        const auto list=rows();require(index>=0 && index<static_cast<int>(list.size()),"Invalid Veyl menu selection");const auto& r=list[index];
        if(!r.available) return {};
        if(r.key=="done" || r.key=="cancel") {page=Page::Finished;return {true,std::nullopt,0};}
        if(r.key=="next") {offset+=6;return {};}
        if(r.key=="previous") {offset-=6;return {};}
        if(r.key=="back") {page=page==Page::Confirm && policy.targeted(choice.currency)?Page::Target:Page::Currency;return {};}
        if(page==Page::Currency) {choice={r.key,""};page=policy.targeted(r.key)?Page::Target:Page::Confirm;}
        else if(page==Page::Target) {choice.target=r.key;page=Page::Confirm;}
        else if(page==Page::Confirm) {const auto* o=selected();require(o && o->at("affordable").get<bool>(),"Veyl is unaffordable");return {false,choice,o->at("cost")};}
        return {};
    }
};
}
