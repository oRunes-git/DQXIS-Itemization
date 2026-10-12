#pragma once
#include "forge_policy.hpp"
#include <set>
#include <tuple>
namespace itemization {
// Runtime item revisions are immutable. Cache static eligibility/previews by
// revision and native cost; never cache pearls, ownership or a commit decision.
class ForgeNavigation {
    const ItemService& service;
    const ForgePolicy& policy;
    struct Entry {int minimum_cost=0;std::optional<Json> options;};
    std::map<std::pair<std::string,int>,Entry> entries;
    std::set<std::tuple<std::string,std::string,std::string,std::string>> validated_choices;
public:
    size_t eligibility_builds=0,option_builds=0,performance_validations=0;
    ForgeNavigation(const ItemService& s,const ForgePolicy& p):service(s),policy(p) {}
    int minimum_cost(const std::string& revision,int native_cost) {
        const auto key=std::make_pair(revision,native_cost);auto found=entries.find(key);
        if(found!=entries.end()) return found->second.minimum_cost;
        const int result=policy.cheapest_available_cost(service,service.lookup(revision),native_cost);
        entries.emplace(key,Entry{result,std::nullopt});++eligibility_builds;return result;
    }
    const Json& options(const std::string& revision,int native_cost) {
        minimum_cost(revision,native_cost);auto& entry=entries.at({revision,native_cost});
        if(!entry.options) {entry.options=policy.options(service,service.lookup(revision),native_cost);++option_builds;}
        return *entry.options;
    }
    ForgePerformanceDecision performance_decision(const std::string& revision,const ForgeChoice& choice,double score) {
        require(policy.uses_performance() && std::isfinite(score) && score>=0 && score<=100,"Invalid forging performance");
        const auto& item=service.lookup(revision);
        const auto key=std::make_tuple(revision,choice.affix_id,choice.operation,choice.replacement_id);
        if(!validated_choices.count(key)) {
            require(policy.can_apply(service,item,choice),"Selected forging operation is unavailable");
            validated_choices.insert(key);++performance_validations;
        }
        // Live scores are always recalculated, but immutable pool eligibility
        // needs validating only once. Execution still uses the guarded policy.
        return policy.available_performance_decision(service,item,choice,score);
    }
};
}
