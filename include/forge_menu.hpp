#pragma once
#include "forge_policy.hpp"
#include "forge_feedback.hpp"
#include <optional>
#include <iomanip>
#include <sstream>
namespace itemization {
inline std::string forge_performance_result_text(const ItemService& service,const Json& before,const Json& after,
    const ForgeChoice& choice,const ForgePerformanceDecision& decision) {
    require(decision.result>=0 && decision.result<=4,"Invalid displayed forge outcome");
    const auto& old=before.at("affixes");
    const auto target=std::find_if(old.begin(),old.end(),[&](const Json& a) {return a.at("id")==choice.affix_id;});
    require(target!=old.end() && old.size()==after.at("affixes").size(),"Missing displayed forge target");
    const auto& affix=after.at("affixes").at(static_cast<size_t>(target-old.begin()));
    const auto tier=service.affix_tier(affix);
    const std::string unit=affix.at("unit")=="percentage_points"?"%":"";
    if(choice.operation=="reroll_value") return instance_name(after)+"\r\nReforge Value - score "+forge_feedback_score(decision.score)+"/100\r\n"+
        display_stat_label(affix.at("label"))+" "+target->at("value").dump()+unit+" -> "+affix.at("value").dump()+unit+
        " [T"+std::to_string(tier.at("rank").get<int>())+": "+tier.at("minimum").dump()+"-"+tier.at("maximum").dump()+unit+"]<end>";
    std::string result=decision.result==0?"Reworking failed.":decision.result==4?"Perfect rework!":"Reworking succeeded.";
    result+="\r\nScore: "+forge_feedback_score(decision.score)+"/100 (goal "+std::to_string(decision.success_score)+").\r\n";
    if(decision.result==0) {
        if(choice.operation=="replace_affix") result+="Random modifier, lower tier (min T1).\r\n";
        else result+="Item unchanged.\r\n";
    }
    result+=instance_name(after)+"\r\n"+display_stat_label(affix.at("label"))+" +"+affix.at("value").dump()+
        (affix.at("unit")=="percentage_points"?"%":"")+" (T"+std::to_string(tier.at("rank").get<int>())+")<end>";
    return result;
}
// A menu transition never edits an item or spends pearls. The native adapter
// revalidates ownership, eligibility and price after the final confirmation.
class ForgeMenu {
public:
    enum class Stage {Action,Modifier,Category,Replacement,Confirm,Finished};
    struct Row {std::string label,key;bool available=true;};
    struct Selection {bool finished=false;std::optional<ForgeChoice> choice;int cost=0;};
private:
    Json options,modifiers;
    std::string item_name,operation,target,replacement,category;
    Stage page=Stage::Action;
    size_t category_page=0,replacement_page=0,page_capacity=0;
    std::string option_category(const Json& option) const {return option.value("replacement_category",std::string("Other"));}
    void paginate(std::vector<Row>& result,size_t current) const {
        if(!page_capacity || result.size()<=page_capacity) return;
        const size_t total=result.size(),start=current*page_capacity;
        require(start<total,"Invalid forge menu page");const auto end=std::min(total,start+page_capacity);
        result=std::vector<Row>(result.begin()+start,result.begin()+end);
        if(end<total) result.push_back({"Next page","next_page"});
        if(start>0) result.push_back({"Previous page","previous_page"});
    }
    const Json* selected_option() const {
        for (const auto& option:options)
            if (option.at("operation")==operation && option.at("affix_id")==target &&
                option.value("replacement_id",std::string{})==replacement) return &option;
        return nullptr;
    }
    bool has_options(const std::string& action,const std::string& affix={}) const {
        for (const auto& option:options)
            if (option.at("operation")==action && (affix.empty() || option.at("affix_id")==affix)) return true;
        return false;
    }
    static std::string number(const Json& value) {return std::to_string(value.get<int>());}
    static std::string unit_text(const Json& modifier) {
        return modifier.value("unit",std::string{})=="percentage_points"?"%":"";
    }
    static std::string side_text(const std::string& kind) {
        require(kind=="prefix" || kind=="suffix","Invalid forge modifier side");
        return kind=="prefix"?"Prefix: ":"Suffix: ";
    }
    static std::string tier_text(const Json& tier,const std::string& unit={}) {
        return "T"+number(tier.at("rank"))+": "+number(tier.at("minimum"))+"-"+number(tier.at("maximum"))+unit;
    }
    std::string target_text() const {
        for (const auto& modifier:modifiers) if (modifier.at("affix_id")==target)
            return side_text(modifier.at("kind"))+display_stat_label(modifier.at("label"))+" +"+number(modifier.at("value"))+unit_text(modifier)+" ["+tier_text(modifier.at("tier"),unit_text(modifier))+"]";
        return {};
    }
public:
    ForgeMenu(const ItemService& service,const Json& item,Json affordable_options,size_t capacity=0):options(std::move(affordable_options)),modifiers(Json::array()),item_name(instance_name(item)),page_capacity(capacity) {
        for (const auto& affix:item.at("affixes")) modifiers.push_back({{"affix_id",affix.at("id")},
            {"label",affix.at("label")},{"kind",affix.at("kind")},{"unit",affix.at("unit")},{"value",affix.at("value")},{"tier",service.affix_tier(affix)}});
    }
    Stage stage() const {return page;}
    std::string stage_name() const {
        switch(page) {case Stage::Action:return "action";case Stage::Modifier:return "modifier";
            case Stage::Category:return "category";
            case Stage::Replacement:return "replacement";case Stage::Confirm:return "confirm";default:return "finished";}
    }
    std::vector<Row> rows() const {
        std::vector<Row> result;
        if (page==Stage::Finished) return result;
        if (page==Stage::Action) {
            const std::array<std::pair<const char*,const char*>,4> actions{{
                {"reroll_value","Reforge Value"},{"replace_affix","Replace modifier"},
                {"reroll_tier","Reroll tier"},{"upgrade_tier","Upgrade tier"}}};
            for (const auto& action:actions) if(has_options(action.first)) {
                int price=0;for(const auto& option:options) if(option.at("operation")==action.first) {price=option.at("cost");break;}
                result.push_back({std::string(action.second)+" ("+std::to_string(price)+" pearls)",action.first});
            }
        } else if(page==Stage::Modifier) {
            // Show every modifier on the item. An ineligible slot remains visible
            // but selecting it cannot advance or charge for an unavailable action.
            for(const auto& modifier:modifiers) {
                const std::string id=modifier.at("affix_id");const bool eligible=has_options(operation,id);
                result.push_back({side_text(modifier.at("kind"))+display_stat_label(modifier.at("label"))+" +"+number(modifier.at("value"))+unit_text(modifier)+
                    " ["+tier_text(modifier.at("tier"),unit_text(modifier))+"]"+(eligible?"":" - unavailable"),id,eligible});
            }
        } else if(page==Stage::Category) {
            std::vector<std::string> order;std::map<std::string,size_t> counts;
            for(const auto& option:options) if(option.at("operation")==operation && option.at("affix_id")==target) {
                const auto group=option_category(option);if(!counts.count(group)) order.push_back(group);++counts[group];
            }
            for(const auto& group:order) result.push_back({group+" ("+std::to_string(counts.at(group))+")",group});
            paginate(result,category_page);
        } else if(page==Stage::Replacement) {
            for(const auto& option:options) if(option.at("operation")==operation && option.at("affix_id")==target && option_category(option)==category)
                result.push_back({side_text(option.at("replacement_kind"))+display_stat_label(option.at("replacement_label"))+" ["+tier_text(option.at("replacement_tier"),
                    option.value("replacement_unit",std::string{})=="percentage_points"?"%":"")+"]",option.at("replacement_id")});
            paginate(result,replacement_page);
        } else if(page==Stage::Confirm) result.push_back({"Start forging","confirm"});
        if(page!=Stage::Action) result.push_back({page==Stage::Replacement?"Back to categories":"Back","back"});
        result.push_back({"Cancel","cancel"});return result;
    }
    std::string body_text() const {
        std::string result=item_name+"\r\n";
        if(page!=Stage::Confirm) result+="Choose an action and modifier.\r\nBrowse categories to choose a replacement.\r\n";
        else if(page==Stage::Confirm) {
            const auto* option=selected_option();require(option!=nullptr,"Missing confirmed forge option");
            result+=target_text()+"\r\n"+option->at("operation_label").get<std::string>()+" ("+number(option->at("cost"))+" pearls).\r\n";
            if(option->contains("performance")) {
                const auto& difficulty=option->at("performance");
                if(operation=="reroll_value") {
                    // Units come from the actual selected modifier, not the operation.
                    std::string target_unit;for(const auto& m:modifiers) if(m.at("affix_id")==target) target_unit=unit_text(m);
                    const auto& tier=option->at("tier");
                    result+="Score 0: "+number(tier.at("minimum"))+target_unit+" | Max "+number(tier.at("maximum"))+target_unit+" at "+number(difficulty.at("perfect_score"))+".\r\n";
                    if(option->at("value").get<int>()<tier.at("minimum").get<int>() || option->at("value").get<int>()>tier.at("maximum").get<int>())
                        result+="Uses the current tier range on completion.\r\n";
                    else result+="Below "+number(difficulty.at("success_score"))+": lowers value (min "+number(tier.at("minimum"))+target_unit+").\r\n";
                } else result+="Score "+number(difficulty.at("success_score"))+"+ | Perfect "+number(difficulty.at("perfect_score"))+"+.\r\n";
                if(operation=="reroll_tier") {
                    result+="Higher tiers need better scores and are rarer.\r\n";
                    const auto& thresholds=difficulty.at("tier_thresholds");
                    if(thresholds.size()<=3) {
                        result+="Reach: ";bool first=true;
                        for(const auto& threshold:thresholds) {if(!first) result+=", ";first=false;result+="T"+number(threshold.at("rank"))+" at "+number(threshold.at("minimum_score"));}
                        result+=".\r\n";
                    } else result+="Top tier needs "+number(thresholds.back().at("minimum_score"))+"; a tier roll can go down.\r\n";
                }
            }
            if(operation=="replace_affix") result+="Success: chosen modifier; perfect improves it.\r\nFailure: random modifier, lower tier (min 1).\r\n";
            else if(operation=="reroll_value" && !option->contains("performance")) result+="Failure gives the tier minimum.\r\n";
            else if(operation!="reroll_value") result+="Failure: item unchanged.\r\n";
        }
        return result;
    }
    std::string text() const {
        std::string result=body_text()+"<select>\r\n";for(const auto& row:rows()) result+=row.label+"\r\n";
        return result+"<select_end><end>";
    }
    Selection select(int index) {
        const auto choices=rows();require(index>=0 && index<static_cast<int>(choices.size()),"Invalid crafting menu choice");
        const auto& row=choices.at(index);
        if(!row.available) return {};
        if(row.key=="next_page") {if(page==Stage::Category) ++category_page;else ++replacement_page;return {};}
        if(row.key=="previous_page") {auto& current=page==Stage::Category?category_page:replacement_page;require(current>0,"No previous forge page");--current;return {};}
        if(row.key=="cancel") {page=Stage::Finished;return {true,{},0};}
        if(row.key=="back") {
            if(page==Stage::Modifier) {page=Stage::Action;operation.clear();target.clear();replacement.clear();}
            else if(page==Stage::Category) {page=Stage::Modifier;target.clear();category.clear();replacement.clear();category_page=0;replacement_page=0;}
            else if(page==Stage::Replacement) {page=Stage::Category;category.clear();replacement.clear();replacement_page=0;}
            else if(page==Stage::Confirm) {page=operation=="replace_affix"?Stage::Replacement:Stage::Modifier;replacement.clear();}
            return {};
        }
        if(page==Stage::Action) {operation=row.key;page=Stage::Modifier;}
        else if(page==Stage::Modifier) {target=row.key;category_page=0;replacement_page=0;page=operation=="replace_affix"?Stage::Category:Stage::Confirm;}
        else if(page==Stage::Category) {category=row.key;replacement_page=0;page=Stage::Replacement;}
        else if(page==Stage::Replacement) {replacement=row.key;page=Stage::Confirm;}
        else if(page==Stage::Confirm) {
            const auto* option=selected_option();require(option!=nullptr,"Forge selection is no longer available");
            ForgeChoice choice{target,operation,replacement};int cost=option->at("cost");page=Stage::Finished;
            return {true,choice,cost};
        }
        return {};
    }
};
// Native failure calls ReplaceRetryKaziItem with the original ID, unlike success.
// Accept that alias only for a validated failed replacement/value reroll; commit the
// prepared revision. Session/source/ownership checks remain in the adapter.
inline bool forge_commit_request_matches(uint64_t requested,uint64_t source,uint64_t prepared,int outcome,const std::string& operation) {
    return requested==prepared || (outcome==0 && (operation=="replace_affix" || operation=="reroll_value" || operation=="quality") && requested==source);
}
}
