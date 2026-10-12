#pragma once
#include "itemization.hpp"
#include <cmath>

namespace itemization {
struct ForgeGauge {double lower=0,upper=0,target=0,value=0;};
struct ForgePerformance {double score=0,worst_deviation=0;int gauges=0,exact=0;};
struct ForgeRankDifficulty {
    int rank=0,edit_success=0,edit_perfect=0,upgrade_success=0,upgrade_perfect=0,tier_reroll_minimum=0;
    uint32_t tier_reroll_weight=0;
};
class ForgePerformanceRules {
    std::map<int,ForgeRankDifficulty> ranks;
public:
    int replacement_surcharge=0,tier_reroll_perfect=95;
    double exact_bonus=2,outside_penalty=1.5;
    static ForgePerformanceRules parse(const Json& data) {
        require(data.at("schema_version")==1,"Unsupported forge performance rules");
        ForgePerformanceRules result;
        result.replacement_surcharge=data.at("replacement_score_surcharge");
        result.tier_reroll_perfect=data.at("tier_reroll_perfect_score");
        result.exact_bonus=data.at("exact_hit_score_bonus");
        result.outside_penalty=data.at("outside_green_minimum_deviation");
        require(result.replacement_surcharge>=0 && result.replacement_surcharge<=20 &&
            result.tier_reroll_perfect>=1 && result.tier_reroll_perfect<=100 &&
            std::isfinite(result.exact_bonus) && result.exact_bonus>=0 && result.exact_bonus<=5 &&
            std::isfinite(result.outside_penalty) && result.outside_penalty>1 && result.outside_penalty<=5,"Invalid performance settings");
        ForgeRankDifficulty previous;
        for(const auto& row:data.at("rank_rules")) {
            ForgeRankDifficulty rule;
            rule.rank=row.at("rank");rule.edit_success=row.at("edit_success");rule.edit_perfect=row.at("edit_perfect");
            rule.upgrade_success=row.at("upgrade_success");rule.upgrade_perfect=row.at("upgrade_perfect");
            rule.tier_reroll_minimum=row.at("tier_reroll_minimum");rule.tier_reroll_weight=row.at("tier_reroll_weight");
            require(rule.rank==previous.rank+1 && rule.rank<=100,"Difficulty ranks must be contiguous");
            require(rule.edit_success>=1 && rule.edit_success+result.replacement_surcharge<=rule.edit_perfect &&
                rule.edit_perfect<=100 && rule.upgrade_success>=rule.edit_success &&
                rule.upgrade_success<=rule.upgrade_perfect && rule.upgrade_perfect<=100 &&
                rule.tier_reroll_minimum>=1 && rule.tier_reroll_minimum<=100 && rule.tier_reroll_weight>0 && rule.tier_reroll_weight<=1000000,
                "Invalid rank difficulty or tier weight");
            if(previous.rank) require(rule.edit_success>=previous.edit_success && rule.edit_perfect>=previous.edit_perfect &&
                rule.upgrade_success>=previous.upgrade_success && rule.upgrade_perfect>=previous.upgrade_perfect &&
                rule.tier_reroll_minimum>=previous.tier_reroll_minimum && rule.tier_reroll_weight<=previous.tier_reroll_weight,
                "Higher ranks must increase difficulty and reduce tier-reroll weight");
            result.ranks.emplace(rule.rank,rule);previous=rule;
        }
        require(!result.ranks.empty(),"No forge difficulty ranks");return result;
    }
    const ForgeRankDifficulty& at(int rank) const {
        require(ranks.count(rank)!=0,"Modifier rank has no difficulty definition");return ranks.at(rank);
    }
    int maximum_rank() const {return ranks.rbegin()->first;}
    double deviation(const ForgeGauge& gauge) const {
            for(const double value:{gauge.lower,gauge.upper,gauge.target,gauge.value})
                require(std::isfinite(value) && std::abs(value)<=1000000,"Invalid forge gauge value");
            require(gauge.lower<gauge.upper && gauge.lower<=gauge.target && gauge.target<=gauge.upper,"Forge target outside its tolerance");
            double radius=gauge.value>=gauge.target?gauge.upper-gauge.target:gauge.target-gauge.lower;
            // Native intervals are inclusive. An endpoint target has no green
            // tolerance on one side; an outside finish still incurs the penalty.
            if(radius==0) radius=gauge.upper-gauge.lower;
            double deviation=std::abs(gauge.value-gauge.target)/radius;
            if(gauge.value<gauge.lower || gauge.value>gauge.upper) deviation=std::max(deviation,outside_penalty);
            return deviation;
    }
    ForgePerformance measure(const std::vector<ForgeGauge>& gauges) const {
        require(!gauges.empty() && gauges.size()<=8,"Invalid active forge gauges");
        ForgePerformance result;double total=0;
        for(const auto& gauge:gauges) {
            const auto distance=deviation(gauge);
            total+=distance;result.worst_deviation=std::max(result.worst_deviation,distance);
            ++result.gauges;if(gauge.value==gauge.target) ++result.exact;
        }
        result.score=std::clamp(100-50*total/result.gauges+exact_bonus*result.exact/result.gauges,0.0,100.0);
        // Never let an exact-hit bonus counterfeit the all-target finish needed
        // for the rarest tier-reroll results.
        if(result.exact!=result.gauges) result.score=std::min(99.0,result.score);
        return result;
    }
};
struct ForgePerformanceDecision {
    double score=0;
    int result=0,challenge_rank=1,success_score=0,perfect_score=0;
    std::vector<int> reachable_ranks;
    Json details() const {return {{"score",score},{"result",result},{"challenge_rank",challenge_rank},
        {"success_score",success_score},{"perfect_score",perfect_score},{"reachable_ranks",reachable_ranks}};}
};
inline int forge_reforged_value(const Json& tier,int current,const ForgePerformanceDecision& decision) {
    const int minimum=tier.at("minimum"),maximum=tier.at("maximum");
    require(minimum<=maximum && current>=0 && current<=100000 && std::isfinite(decision.score) && decision.score>=0 && decision.score<=100 &&
        decision.perfect_score>0 && decision.perfect_score<=100,"Invalid value reforging projection");
    // A preserved historical roll may lie outside today's universal tier range.
    // Use its nearest bound only for the explicit reforge; loading is unchanged.
    current=std::clamp(current,minimum,maximum);
    int value=decision.score>=decision.perfect_score?maximum:
        minimum+static_cast<int>(std::floor((maximum-minimum)*decision.score/decision.perfect_score));
    if(decision.result==0) value=std::min(value,std::max(minimum,current-1));
    return value;
}
}
