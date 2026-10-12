#pragma once
#include "veyl.hpp"
#include "camp_crafting.hpp"
#include <array>
namespace itemization {
enum class CraftScreenKind {Crafting,Salvage};
enum class CraftCurrencyTab {Veyls,Prefixes,Suffixes};
constexpr size_t craft_currency_rows=8;
enum class CraftScreenStage {Equipment,Currency,Target,Confirm,Result,Closed};
struct CraftScreenRow {std::string key,label;bool available=true;std::string native_id;int amount=0;};
struct CraftScreenRequest {
    std::string source,shard;std::optional<VeylChoice> veyl;int cost=0;
    std::vector<std::string> salvage;
    bool empty() const {return !veyl && shard.empty() && salvage.empty();}
};
// Entirely independent of forge state. Rendering and native transactions are
// adapters; the model never consumes inventory, starts a minigame or rolls RNG.
class CraftingScreen {
    const ItemService& service;const VeylPolicy& veyls;const CampCraftPolicy& camp;
    CraftScreenKind kind_;CraftScreenStage stage_=CraftScreenStage::Equipment;
    std::vector<Json> gear_;Json wallet_,shards_;size_t gear_cursor_=0,choice_cursor_=0;
    CraftCurrencyTab currency_tab_=CraftCurrencyTab::Veyls;std::string currency_,target_,result_;int quote_=0;
    std::set<std::string> selected_;bool issued_=false;
    mutable std::map<std::string,bool> availability_;
    mutable std::map<std::string,std::set<std::string>> shard_candidates_;
    mutable std::string rows_key_,explanation_key_,wallet_text_;
    mutable bool rows_valid_=false,explanation_valid_=false,wallet_valid_=false;
    mutable std::vector<CraftScreenRow> rows_cache_;
    mutable std::map<std::string,std::string> details_cache_;
    mutable std::string explanation_cache_;
    size_t selection_revision_=0;
    mutable size_t row_builds_=0,detail_builds_=0,explanation_builds_=0;
    struct SalvageChance {int rolls=0,bonus_rolls=0;};
    std::map<std::string,SalvageChance> salvage_chances_;
    std::map<std::string,std::vector<std::pair<std::string,int>>> salvage_item_chances_;
    void update_salvage_chances(const Json& gear,bool add) {
        const std::string id=gear.at("engine_id");auto found=salvage_item_chances_.find(id);
        if(found==salvage_item_chances_.end()) {
            std::vector<std::pair<std::string,int>> chances;
            for(const auto& p:camp.salvage_preview(service,{gear})) chances.emplace_back(p.at("family_id"),p.at("bonus_rolls"));
            found=salvage_item_chances_.emplace(id,std::move(chances)).first;
        }
        for(const auto& p:found->second) {
            if(add) {auto& c=salvage_chances_[p.first];++c.rolls;c.bonus_rolls+=p.second;}
            else {auto& c=salvage_chances_.at(p.first);--c.rolls;c.bonus_rolls-=p.second;if(!c.rolls)salvage_chances_.erase(p.first);}
        }
    }
    const Json& item() const {require(!gear_.empty(),"No equipment selected");return gear_.at(gear_cursor_);}
    bool currency_available(const std::string& id) const {
        if(gear_.empty()) return false;
        const auto key=source()+":V:"+id;const auto found=availability_.find(key);if(found!=availability_.end())return found->second;
        if(veyls.targeted(id)) {
            for(const auto& a:item().at("affixes")) {
                const VeylChoice c{id,a.at("id")};
                if(veyls.eligible(service,item(),c) && wallet_.value(id,0)>=veyls.cost(service,item(),c)) return availability_[key]=true;
            }return availability_[key]=false;
        }
        const VeylChoice c{id,""};return availability_[key]=veyls.eligible(service,item(),c) && wallet_.value(id,0)>=veyls.cost(service,item(),c);
    }
    bool shard_available(const std::string& id) const {
        if(gear_.empty())return false;const auto key=source()+":S:"+id;
        const auto found=availability_.find(key);if(found!=availability_.end())return found->second;
        const auto source_id=source();auto candidates=shard_candidates_.find(source_id);
        if(candidates==shard_candidates_.end())candidates=shard_candidates_.emplace(source_id,service.shard_craftable_affix_families(item())).first;
        return availability_[key]=candidates->second.count(id)!=0;
    }
    void bound_choice() {const auto& rows=choices();choice_cursor_=rows.empty()?0:std::min(choice_cursor_,rows.size()-1);}
    bool confirmation_available() const {
        if(issued_)return false;
        if(kind_==CraftScreenKind::Salvage)return !selected_.empty();
        if(gear_.empty()||currency_.empty())return false;
        if(shard_tab())return shards_.value(currency_,0)>=1&&shard_available(currency_);
        const VeylChoice choice{currency_,target_};
        return veyls.eligible(service,item(),choice)&&wallet_.value(currency_,0)>=veyls.cost(service,item(),choice);
    }
public:
    CraftingScreen(const ItemService& s,const VeylPolicy& v,const CampCraftPolicy& c,CraftScreenKind k,
        std::vector<Json> gear,Json wallet,Json shards):service(s),veyls(v),camp(c),kind_(k),gear_(std::move(gear)),wallet_(std::move(wallet)),shards_(std::move(shards)) {
        if(kind_==CraftScreenKind::Salvage) gear_.erase(std::remove_if(gear_.begin(),gear_.end(),[&](const Json& g){return g.value("ui_equipped",false)||camp.protected_item(service,g);}),gear_.end());
    }
    CraftScreenKind kind() const {return kind_;}CraftScreenStage stage() const {return stage_;}
    bool shard_tab() const {return currency_tab_!=CraftCurrencyTab::Veyls;}
    CraftCurrencyTab currency_tab() const {return currency_tab_;}size_t gear_cursor() const {return gear_cursor_;}
    bool select_source(const std::string& id) {
        for(size_t i=0;i<gear_.size();++i)if(gear_[i].at("engine_id")==id){if(gear_cursor_!=i)choice_cursor_=0;gear_cursor_=i;return true;}
        return false;
    }
    size_t choice_cursor() const {return choice_cursor_;}const std::vector<Json>& gear() const {return gear_;}
    std::string source() const {return gear_.empty()?"":item().at("engine_id").get<std::string>();}
    const Json* selected_item() const {return gear_.empty()?nullptr:&item();}
    bool selected(const std::string& id) const {return selected_.count(id)!=0;}
    size_t selection_count() const {return selected_.size();}
    std::array<size_t,3> presentation_builds() const {return {row_builds_,detail_builds_,explanation_builds_};}
    std::string wallet_summary() const {
        if(kind_==CraftScreenKind::Salvage) return "";
        if(wallet_valid_) return wallet_text_;
        std::string text;size_t n=0;
        for(const auto& c:veyls.currencies()) {
            if(!veyls.visible(c.at("id"),wallet_))continue;
            const auto name=veyl_short_name(c.at("name"));
            if(n) text+=n%4==0?"\n":"   ";++n;text+=name+" "+std::to_string(wallet_.value(c.at("id").get<std::string>(),0));
        }wallet_text_=text;wallet_valid_=true;return text;
    }
    std::string details() const {
        if(gear_.empty()) return "No eligible equipment in your inventory.";
        const auto key=source();auto found=details_cache_.find(key);
        if(found==details_cache_.end()) {++detail_builds_;found=details_cache_.emplace(key,item_description(service,item())).first;}
        return found->second;
    }
    const std::vector<CraftScreenRow>& choices() const {
        const auto key=(kind_==CraftScreenKind::Salvage?std::string("salvage"):source())+":"+std::to_string(static_cast<int>(stage_))+":"+std::to_string(static_cast<int>(currency_tab_))+":"+currency_+":"+std::to_string(selected_.size())+":"+std::to_string(issued_);
        if(!rows_valid_ || rows_key_!=key) {rows_cache_=build_choices();rows_key_=key;rows_valid_=true;++row_builds_;}
        return rows_cache_;
    }
private:
    std::vector<CraftScreenRow> build_choices() const {
        std::vector<CraftScreenRow> rows;
        if(stage_==CraftScreenStage::Result) return {{"done","Continue",true}};
        if(stage_==CraftScreenStage::Closed) return {};
        if(stage_==CraftScreenStage::Confirm) return {{"apply",kind_==CraftScreenKind::Salvage?"Salvage selected equipment":"Apply craft",confirmation_available()},{"back","Back",true}};
        if(kind_==CraftScreenKind::Salvage) return {{"review","Review "+std::to_string(selected_.size())+" selected",!selected_.empty()},{"clear","Clear selection",!selected_.empty()}};
        if(stage_==CraftScreenStage::Target) {
            for(const auto& a:item().at("affixes")) {
                const std::string id=a.at("id");const VeylChoice c{currency_,id};
                bool okay=veyls.eligible(service,item(),c);int cost=0;
                if(okay) {cost=veyls.cost(service,item(),c);okay=wallet_.value(currency_,0)>=cost;}
                rows.push_back({id,std::string(a.at("kind")=="prefix"?"Prefix: ":"Suffix: ")+display_stat_label(a.at("label"))+" [T"+
                    service.affix_tier(a).at("rank").dump()+"]  Cost "+std::to_string(cost),okay});
            }return rows;
        }
        if(shard_tab()) {
            for(const auto& s:camp.shards()) {
                if(s.at("kind")!=(currency_tab_==CraftCurrencyTab::Prefixes?"prefix":"suffix"))continue;
                const std::string id=s.at("family_id");const int n=shards_.value(id,0);if(!n) continue;
                rows.push_back({id,s.at("name"),shard_available(id),s.at("native_id"),n});
            }
            std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.available!=b.available?a.available>b.available:a.label<b.label;});
        } else for(const auto& c:veyls.currencies()) {
            const std::string id=c.at("id");if(!veyls.visible(id,wallet_))continue;
            rows.push_back({id,c.at("name"),currency_available(id),c.at("native_id"),wallet_.value(id,0)});
        }
        return rows;
    }
    std::string build_explanation() const {
        if(stage_==CraftScreenStage::Result) return result_;
        if(kind_==CraftScreenKind::Salvage) {
            std::string text="Selected: "+std::to_string(selected_.size())+" items.\nEquipped, locked and protected gear is excluded.";
            if(!selected_.empty()) {
                text=stage_==CraftScreenStage::Confirm?"Permanently salvage "+std::to_string(selected_.size())+" items?\nShards are rolled independently; no drop is guaranteed.":text;
                for(const auto& p:salvage_chances_) {const auto& c=p.second;
                    text+="\n"+camp.definition(p.first).at("name").get<std::string>()+": "+std::to_string(c.rolls)+" x "+display_number(camp.base_chance()/100.0)+"%"+
                        (c.bonus_rolls?" + "+std::to_string(c.bonus_rolls)+" x "+display_number(camp.bonus_chance()/100.0)+"%":"")+
                        ", expected "+display_number((c.rolls*camp.base_chance()+c.bonus_rolls*camp.bonus_chance())/10000.0);}
            }return text;
        }
        const auto& rows=choices();if(rows.empty()) return shard_tab()?(currency_tab_==CraftCurrencyTab::Prefixes?"No prefix shards owned. Salvage equipment at a campsite to find shards.":"No suffix shards owned. Salvage equipment at a campsite to find shards."):"Choose equipment.";
        if(stage_==CraftScreenStage::Confirm) {
            std::string text=shard_tab()?camp.craft_description(service,item(),currency_):veyls.definition(currency_).at("description").get<std::string>();
            if(!target_.empty()) for(const auto& a:item().at("affixes")) if(a.at("id")==target_) text+="\nTarget: "+display_stat_label(a.at("label"));
            const auto balance=shard_tab()?shards_.value(currency_,0):wallet_.value(currency_,0);
            return text+"\nCost: "+std::to_string(quote_)+". Owned: "+std::to_string(balance)+"."+
                (confirmation_available()?" Applies instantly.":"");
        }
        if(stage_==CraftScreenStage::Target) return "Choose the modifier to change.";
        const auto& row=rows.at(std::min(choice_cursor_,rows.size()-1));
        std::string text=shard_tab()?camp.craft_description(service,item(),row.key):veyls.definition(row.key).at("description").get<std::string>();
        return text;
    }
public:
    std::string explanation() const {
        (void)choices();
        const auto key=rows_key_+":"+std::to_string(choice_cursor_)+":"+target_+":"+std::to_string(selection_revision_);
        if(!explanation_valid_ || explanation_key_!=key) {explanation_cache_=build_explanation();explanation_key_=key;explanation_valid_=true;++explanation_builds_;}
        return explanation_cache_;
    }
    void move(int direction,bool page=false) {
        if(stage_==CraftScreenStage::Closed) return;
        const int step=direction*(page?static_cast<int>(craft_currency_rows):1);
        auto advance=[&](size_t& cursor,size_t count) {if(count) cursor=static_cast<size_t>(std::clamp(static_cast<int>(cursor)+step,0,static_cast<int>(count)-1));};
        if(stage_==CraftScreenStage::Equipment) {advance(gear_cursor_,gear_.size());choice_cursor_=0;}
        else advance(choice_cursor_,choices().size());
    }
    void switch_tab(int direction=1) {
        if(kind_!=CraftScreenKind::Crafting || (stage_!=CraftScreenStage::Equipment && stage_!=CraftScreenStage::Currency)) return;
        currency_tab_=static_cast<CraftCurrencyTab>((static_cast<int>(currency_tab_)+(direction<0?2:1))%3);choice_cursor_=0;currency_.clear();target_.clear();
    }
    void right() {if(stage_==CraftScreenStage::Equipment) {stage_=CraftScreenStage::Currency;bound_choice();}}
    void left() {
        if(stage_==CraftScreenStage::Currency && kind_==CraftScreenKind::Crafting) switch_tab(-1);
        else if(stage_!=CraftScreenStage::Equipment) back();
    }
    void back() {
        if(stage_==CraftScreenStage::Equipment) stage_=CraftScreenStage::Closed;
        else if(stage_==CraftScreenStage::Result) {stage_=CraftScreenStage::Equipment;issued_=false;}
        else if(stage_==CraftScreenStage::Confirm) {stage_=!target_.empty()?CraftScreenStage::Target:CraftScreenStage::Currency;choice_cursor_=0;}
        else if(stage_==CraftScreenStage::Target) {stage_=CraftScreenStage::Currency;choice_cursor_=0;target_.clear();}
        else stage_=CraftScreenStage::Equipment;
    }
    CraftScreenRequest enter() {
        if(stage_==CraftScreenStage::Equipment) {
            if(gear_.empty()) return {};
            if(kind_==CraftScreenKind::Salvage) {const auto id=source();if(selected_.count(id)) {update_salvage_chances(item(),false);selected_.erase(id);++selection_revision_;}else if(selected_.size()<static_cast<size_t>(camp.maximum_items())) {update_salvage_chances(item(),true);selected_.insert(id);++selection_revision_;}}
            else right();return {};
        }
        const auto& rows=choices();if(rows.empty()) return {};const auto row=rows.at(std::min(choice_cursor_,rows.size()-1));if(!row.available) return {};
        if(row.key=="done") {back();return {};}
        if(row.key=="back") {back();return {};}
        if(row.key=="clear") {selected_.clear();salvage_chances_.clear();++selection_revision_;return {};}
        if(row.key=="review") {stage_=CraftScreenStage::Confirm;choice_cursor_=0;return {};}
        if(row.key=="apply") {
            if(issued_) return {};issued_=true;stage_=CraftScreenStage::Result;
            CraftScreenRequest request;request.source=source();request.cost=quote_;
            if(kind_==CraftScreenKind::Salvage) request.salvage={selected_.begin(),selected_.end()};
            else if(shard_tab()) request.shard=currency_;else request.veyl=VeylChoice{currency_,target_};return request;
        }
        if(stage_==CraftScreenStage::Target) {target_=row.key;quote_=veyls.cost(service,item(),{currency_,target_});stage_=CraftScreenStage::Confirm;}
        else {currency_=row.key;target_.clear();if(!shard_tab() && veyls.targeted(currency_)) stage_=CraftScreenStage::Target;
            else {quote_=shard_tab()?1:veyls.cost(service,item(),{currency_,""});stage_=CraftScreenStage::Confirm;}}
        choice_cursor_=0;return {};
    }
    void refresh(std::vector<Json> gear,Json wallet,Json shards,std::string message,std::string result_id={},bool resume_currency=false) {
        const auto previous=source();gear_=std::move(gear);wallet_=std::move(wallet);shards_=std::move(shards);gear_cursor_=0;
        const auto wanted=result_id.empty()?previous:result_id;
        for(size_t i=0;i<gear_.size();++i) if(gear_[i].at("engine_id")==wanted) gear_cursor_=i;
        selected_.clear();salvage_chances_.clear();salvage_item_chances_.clear();++selection_revision_;availability_.clear();shard_candidates_.clear();details_cache_.clear();
        rows_valid_=explanation_valid_=wallet_valid_=false;result_=std::move(message);stage_=CraftScreenStage::Result;choice_cursor_=0;
        // Only a committed currency transaction rearms confirmation. Keep the
        // revised item and exact modifier family; never silently retarget.
        if(resume_currency&&kind_==CraftScreenKind::Crafting&&!result_id.empty()&&source()==result_id) {
            issued_=false;stage_=CraftScreenStage::Confirm;
            if(shard_tab())quote_=1;
            else {const VeylChoice choice{currency_,target_};
                if(veyls.eligible(service,item(),choice))quote_=veyls.cost(service,item(),choice);}
            if(!confirmation_available()) {
                // Exhausted currencies and completed modifier targets return
                // directly to their list. Shards with zero balance disappear.
                stage_=CraftScreenStage::Currency;target_.clear();quote_=0;
                const auto& rows=choices();
                for(size_t i=0;i<rows.size();++i)if(rows[i].key==currency_){choice_cursor_=i;break;}
            }
        }
    }
    void refresh_equipment(std::vector<Json> gear) {
        const auto previous=source();gear_=std::move(gear);gear_cursor_=0;
        if(kind_==CraftScreenKind::Salvage)gear_.erase(std::remove_if(gear_.begin(),gear_.end(),[&](const Json& g){return g.value("ui_equipped",false)||camp.protected_item(service,g);}),gear_.end());
        for(size_t i=0;i<gear_.size();++i)if(gear_[i].at("engine_id")==previous)gear_cursor_=i;
        const auto previous_selection=selected_;selected_.clear();salvage_chances_.clear();salvage_item_chances_.clear();++selection_revision_;
        for(const auto& g:gear_)if(previous_selection.count(g.at("engine_id"))){selected_.insert(g.at("engine_id"));update_salvage_chances(g,true);}
        details_cache_.clear();rows_valid_=explanation_valid_=false;
        stage_=CraftScreenStage::Equipment;choice_cursor_=0;issued_=false;
    }
};
}
