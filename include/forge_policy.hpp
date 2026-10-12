#pragma once
#include "item_lifecycle.hpp"
#include <array>
#include <optional>
#include "forge_performance.hpp"
#include "item_display.hpp"
namespace itemization {
struct ForgeChoice {std::string affix_id,operation,replacement_id;};
class ForgePolicy {
    friend class ForgeNavigation;
    std::array<int,5> draws{};
    std::map<std::string,int> multipliers;
    int maximum_rank=1;
    std::optional<ForgePerformanceRules> performance;
public:
    static ForgePolicy parse(const Json& data) {
        require(data.at("schema_version")==3 && data.at("quality_draws_by_native_result").size()==5,"Invalid targeted forge policy schema");
        require(data.at("replacement_outcomes_by_native_result")==Json::array({"failure","success","success","success","perfect"}),"Replacement outcomes must match the verified native forge enum");
        require(data.at("perfect_replacement")=="upgrade_one_tier_else_max_value" &&
            data.at("failed_replacement")=="random_affix_strictly_lower_tier_floor_one","Unsupported replacement outcome policy");
        ForgePolicy result;result.maximum_rank=data.at("maximum_tier_rank");
        require(result.maximum_rank>=1 && result.maximum_rank<=100,"Invalid maximum forge tier");
        for (size_t i=0;i<5;++i) {
            result.draws[i]=data.at("quality_draws_by_native_result")[i];
            require(result.draws[i]>=1 && result.draws[i]<=16,"Invalid forge quality bias");
            if (i>0) require(result.draws[i]>=result.draws[i-1],"Better forging must not weaken roll quality");
        }
        for (const auto& operation:{"reroll_value","replace_affix","reroll_tier","upgrade_tier"}) {
            const int multiplier=data.at("pearl_cost_multipliers").at(operation);
            require(multiplier>=1 && multiplier<=100,"Invalid crafting pearl multiplier");
            result.multipliers.emplace(operation,multiplier);
        }
        require(data.value("failed_value_reroll",std::string("performance_scaled_below_previous_floor_minimum"))=="performance_scaled_below_previous_floor_minimum","Unsupported failed value reforging policy");
        require(data.at("failed_other_reworks_keep_item").get<bool>(),"Failed tier edits must preserve the item");
        if(data.contains("performance_difficulty")) {
            result.performance=ForgePerformanceRules::parse(data.at("performance_difficulty"));
            require(result.maximum_rank<=result.performance->maximum_rank(),"Forge cap exceeds configured performance ranks");
        }
        return result;
    }
    int cost(int native_cost,const std::string& operation) const {
        require(native_cost>0 && multipliers.count(operation)!=0,"Invalid crafting cost request");
        const int64_t amount=int64_t(native_cost)*multipliers.at(operation);
        require(amount<=9999,"Crafting cost exceeds pearl capacity");return static_cast<int>(amount);
    }
    bool can_apply(const ItemService& service,const Json& item,const ForgeChoice& choice) const {
        require(multipliers.count(choice.operation)!=0,"Unknown forge operation");
        if (choice.operation=="replace_affix") {
            const auto candidates=service.replacement_options(item,choice.affix_id,maximum_rank);
            return std::any_of(candidates.begin(),candidates.end(),[&](const Json& option) {return option.at("id")==choice.replacement_id;});
        }
        require(choice.replacement_id.empty(),"A replacement target was supplied for another forge operation");
        return service.can_craft(item,choice.affix_id,choice.operation,maximum_rank);
    }
    int cheapest_available_cost(const ItemService& service,const Json& item,int native_cost) const {
        validate_instance(item);require(native_cost>0,"Invalid crafting cost request");
        std::vector<std::pair<int,std::string>> ordered;
        for(const auto& operation:multipliers) ordered.emplace_back(operation.second,operation.first);
        std::sort(ordered.begin(),ordered.end());
        // Inventory eligibility needs one available operation, not every
        // replacement's range and performance preview. Pearls remain live.
        for(const auto& operation:ordered) for(const auto& affix:item.at("affixes"))
            if(service.can_craft(item,affix.at("id"),operation.second,maximum_rank)) return cost(native_cost,operation.second);
        return 0;
    }
    Json options(const ItemService& service,const Json& item,int native_cost) const {
        validate_instance(item);Json result=Json::array();
        const std::array<std::pair<const char*,const char*>,4> operations{{
            {"reroll_value","Reforge Value"},{"replace_affix","Replace modifier"},
            {"reroll_tier","Reroll tier"},{"upgrade_tier","Upgrade tier"}}};
        for (const auto& affix:item.at("affixes")) {
            const auto tier=service.affix_tier(affix);
            for (const auto& operation:operations) {
                ForgeChoice choice{affix.at("id"),operation.first};
                if (choice.operation=="replace_affix") {
                    for (const auto& replacement:service.replacement_options(item,choice.affix_id,maximum_rank))
                        result.push_back({{"affix_id",choice.affix_id},{"operation",choice.operation},
                            {"replacement_id",replacement.at("id")},{"replacement_label",replacement.at("label")},
                            {"replacement_unit",replacement.at("unit")},
                            {"replacement_kind",replacement.at("kind")},
                            {"replacement_category",replacement.at("forge_category")},
                            {"label",affix.at("label")},{"operation_label","Change to "+display_stat_label(replacement.at("label"))},
                            {"tier",tier},{"replacement_tier",replacement.at("tier")},{"value",affix.at("value")},
                            {"cost",cost(native_cost,choice.operation)}});
                    continue;
                }
                if (!can_apply(service,item,choice)) continue;
                result.push_back({{"affix_id",choice.affix_id},{"operation",choice.operation},
                            {"label",affix.at("label")},{"operation_label",operation.second},{"tier",choice.operation=="reroll_value"?service.reforge_tier(item,affix,maximum_rank):tier},
                    {"value",affix.at("value")},{"cost",cost(native_cost,choice.operation)}});
            }
        }
        if(performance) for(auto& option:result) {
            ForgeChoice choice{option.at("affix_id"),option.at("operation"),option.value("replacement_id",std::string{})};
            // These choices were validated above. Re-enumerating the full
            // replacement pool here made menu construction quadratic.
            auto description=available_performance_decision(service,item,choice,100).details();description.erase("score");description.erase("result");
            Json thresholds=Json::array();
            if(choice.operation=="reroll_tier") for(const auto& tier:service.craft_tier_options(item,choice.affix_id,maximum_rank)) {
                const int rank=tier.at("rank");thresholds.push_back({{"rank",rank},{"minimum_score",performance->at(rank).tier_reroll_minimum}});
            }
            description["tier_thresholds"]=thresholds;option["performance"]=description;
        }
        return result;
    }
    bool uses_performance() const {return performance.has_value();}
    ForgePerformance measure(const std::vector<ForgeGauge>& gauges) const {
        require(performance.has_value(),"Performance difficulty is disabled");return performance->measure(gauges);
    }
    ForgePerformanceDecision performance_decision(const ItemService& service,const Json& item,const ForgeChoice& choice,double score) const {
        require(performance.has_value() && std::isfinite(score) && score>=0 && score<=100,"Invalid forging performance");
        require(can_apply(service,item,choice),"Selected forging operation is unavailable");
        return available_performance_decision(service,item,choice,score);
    }
private:
    ForgePerformanceDecision available_performance_decision(const ItemService& service,const Json& item,const ForgeChoice& choice,double score) const {
        const auto& effects=item.at("affixes");
        const auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==choice.affix_id;});
        require(target!=effects.end(),"Performance target is missing");
        ForgePerformanceDecision decision;decision.score=score;decision.challenge_rank=service.affix_tier(*target).at("rank");
        if(choice.operation=="upgrade_tier") decision.challenge_rank=service.next_upgrade_rank(item,*target,maximum_rank);
        const auto& rank=performance->at(decision.challenge_rank);
        if(choice.operation=="reroll_tier") {
            const auto tiers=service.craft_tier_options(item,choice.affix_id,maximum_rank);
            require(!tiers.empty(),"No legal performance tiers");
            decision.success_score=performance->at(tiers.front().at("rank")).tier_reroll_minimum;
            decision.perfect_score=std::max(decision.success_score,performance->tier_reroll_perfect);
            for(const auto& tier:tiers) {
                const int candidate=tier.at("rank");
                if(score>=performance->at(candidate).tier_reroll_minimum) decision.reachable_ranks.push_back(candidate);
            }
        } else if(choice.operation=="upgrade_tier") {
            decision.success_score=rank.upgrade_success;decision.perfect_score=rank.upgrade_perfect;
        } else {
            decision.success_score=rank.edit_success+(choice.operation=="replace_affix"?performance->replacement_surcharge:0);
            decision.perfect_score=rank.edit_perfect;
        }
        if(score>=decision.perfect_score) decision.result=4;
        else if(score>=decision.success_score+(decision.perfect_score-decision.success_score)/2.0) decision.result=3;
        else if(score>=decision.success_score) decision.result=2;
        return decision;
    }
public:
    Json apply_performance(ItemService& service,const Json& item,const ForgeChoice& choice,double score,uint64_t seed) const {
        const auto decision=performance_decision(service,item,choice,score);
        if(choice.operation=="reroll_value") {
            const auto& effects=item.at("affixes");
            const auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==choice.affix_id;});
            require(target!=effects.end(),"Value reforging target is missing");
            const auto tier=service.reforge_tier(item,*target,maximum_rank);
            // Direct, monotonic score-to-value mapping. Perfect is the current
            // rank's goal rather than a flat 100, so high tiers remain harder.
            const int value=forge_reforged_value(tier,target->at("value"),decision);
            return service.reforge_value(item.at("engine_id"),choice.affix_id,value);
        }
        if(choice.operation=="reroll_tier" && decision.result!=0) {
            std::map<int,uint32_t> weights;
            for(const int rank:decision.reachable_ranks) weights.emplace(rank,performance->at(rank).tier_reroll_weight);
            return service.reroll_tier_weighted(item.at("engine_id"),choice.affix_id,weights,seed,draws[decision.result],maximum_rank);
        }
        return apply(service,item,decision.result,choice,seed);
    }
    Json apply(ItemService& service,const Json& item,int native_result,const ForgeChoice& choice,uint64_t seed) const {
        require(native_result>=0 && native_result<5,"Invalid native forge result");
        require(can_apply(service,item,choice),"Selected forge operation is unavailable");
        if (choice.operation=="replace_affix") {
            const auto outcome=native_result==0?AffixReplacementResult::Failure:
                native_result==4?AffixReplacementResult::Perfect:AffixReplacementResult::Success;
            return service.replace_affix(item.at("engine_id"),choice.affix_id,choice.replacement_id,outcome,seed,draws[native_result],maximum_rank);
        }
        // Grade-only fallback for configurations without performance capture:
        // failure always resolves to the tier floor, never an unrelated roll.
        if (native_result==0 && choice.operation=="reroll_value") {
            const auto& effects=item.at("affixes");const auto target=std::find_if(effects.begin(),effects.end(),[&](const Json& a) {return a.at("id")==choice.affix_id;});
            return service.reforge_value(item.at("engine_id"),choice.affix_id,service.reforge_tier(item,*target,maximum_rank).at("minimum"));
        }
        if (native_result==0) return item;
        return service.craft(item.at("engine_id"),choice.affix_id,choice.operation,seed,draws[native_result],maximum_rank);
    }
    Json finish_fresh(ItemService& service,const Json& item,int native_result,uint64_t seed) const {
        require(native_result>=0 && native_result<5,"Invalid native forge result");
        return service.finish_fresh(item,seed,draws[native_result]);
    }
};
}
