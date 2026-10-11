#pragma once

namespace ldcompress {

enum class NativeAnalysisProfile {
    Exact = 0,
    OrderGuessExactRice = 1,
    OrderGuessMeanRice = 2,
    SubdivideTukey3MeanRice = 3,
    OrderGuessMeanEstimateRice = 4,
    SubdivideTukey3MeanEstimateRice = 5,
};

static_assert(static_cast<int>(NativeAnalysisProfile::Exact) == 0);
static_assert(static_cast<int>(NativeAnalysisProfile::OrderGuessExactRice) == 1);
static_assert(static_cast<int>(NativeAnalysisProfile::OrderGuessMeanRice) == 2);
static_assert(static_cast<int>(NativeAnalysisProfile::SubdivideTukey3MeanRice) == 3);
static_assert(static_cast<int>(NativeAnalysisProfile::OrderGuessMeanEstimateRice) == 4);
static_assert(static_cast<int>(NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice) == 5);

inline const char* native_analysis_profile_name(NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return "exact";
    case NativeAnalysisProfile::OrderGuessExactRice:
        return "order-guess-exact-rice";
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return "order-guess-mean-rice";
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        return "order-guess-mean-estimate-rice";
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return "subdivide-tukey3-mean-rice";
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        return "subdivide-tukey3-mean-estimate-rice";
    }
    return "unknown";
}

}  // namespace ldcompress
