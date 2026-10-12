#pragma once
#include "forge_performance.hpp"
#include <iomanip>
#include <sstream>
namespace itemization {
struct ForgeFeedbackBands {double lower,upper,excellent_lower,excellent_upper;};
struct ForgeFeedbackPixels {double lower,upper,excellent_lower,excellent_upper;};
inline ForgeFeedbackBands forge_feedback_bands(const ForgeGauge& gauge) {
    require(std::isfinite(gauge.lower) && std::isfinite(gauge.upper) && std::isfinite(gauge.target) &&
        gauge.lower<gauge.upper && gauge.lower<=gauge.target && gauge.target<=gauge.upper,"Invalid feedback geometry");
    // Base score >=90 means deviation <=0.2 on each side of the target.
    // End-point targets have no excellent area beyond the native safe interval.
    return {gauge.lower,gauge.upper,gauge.target-(gauge.target-gauge.lower)*0.2,
        gauge.target+(gauge.upper-gauge.target)*0.2};
}
inline ForgeFeedbackPixels forge_feedback_pixels(const ForgeGauge& gauge,double maximum,double width,double diamond) {
    const auto bands=forge_feedback_bands(gauge);
    require(std::isfinite(maximum) && maximum>0 && std::isfinite(width) && width>0 &&
        std::isfinite(diamond) && diamond>=0 && diamond<=width && bands.lower>=0 && bands.upper<=maximum,
        "Invalid feedback pixel geometry");
    const double scale=width/maximum,lower=bands.lower*scale,upper=bands.upper*scale;
    // Native diamonds have texture/layout offsets from the raw HP boundary.
    // Keep the original safety edges and project the excellent region around
    // the actual diamond, with the same left/right normalized tolerances.
    return {lower,upper,std::clamp(diamond-(gauge.target-bands.excellent_lower)*scale,lower,upper),
        std::clamp(diamond+(bands.excellent_upper-gauge.target)*scale,lower,upper)};
}
inline std::string forge_feedback_score(double score) {
    require(std::isfinite(score),"Invalid feedback score");
    std::ostringstream text;text<<std::fixed<<std::setprecision(2)<<std::floor(score*100)/100;
    auto result=text.str();
    while(result.back()=='0') result.pop_back();
    if(result.back()=='.') result.pop_back();
    return result;
}
}
