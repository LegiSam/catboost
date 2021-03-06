#include "doc_fstr.h"
#include "feature_str.h"

#include <catboost/libs/model/split.h>
#include <catboost/libs/algo/index_calcer.h>

using TTreeFunction = std::function<void(const TFullModel& model,
                                         const TVector<ui8>& binarizedFeatures,
                                         int treeIdx,
                                         const TFeaturesLayout& layout,
                                         TVector<TVector<double>>* resultPtr)>;

static bool SplitHasFeature(const size_t feature, const TModelSplit& split, const TFeaturesLayout& layout) {
    const EFeatureType featureType = layout.GetExternalFeatureType(feature);
    const int internalIdx = layout.GetInternalFeatureIdx(feature);
    if (split.Type == ESplitType::FloatFeature) {
        return split.FloatFeature.FloatFeature == internalIdx && featureType == EFeatureType::Float;
    } else if (split.Type == ESplitType::OneHotFeature) {
        return split.OneHotFeature.CatFeatureIdx == internalIdx && featureType == EFeatureType::Categorical;
    } else {
        Y_ASSERT(split.Type == ESplitType::OnlineCtr);
        const auto& proj = split.OnlineCtr.Ctr.Base.Projection;
        if (featureType == EFeatureType::Categorical) {
            for (int featureIdx : proj.CatFeatures) {
                if (featureIdx == internalIdx) {
                    return true;
                }
            }
            for (const auto& oneHotFeature : proj.OneHotFeatures) {
                if (oneHotFeature.CatFeatureIdx == internalIdx) {
                    return true;
                }
            }
        } else {
            for (const auto& binFeature : proj.BinFeatures) {
                if (binFeature.FloatFeature == internalIdx) {
                    return true;
                }
            }
        }
    }
    return false;
}

TVector<TVector<TIndexType>> BuildIndicesWithoutFeature(const TFullModel& model,
                                                        const size_t treeId,
                                                        const TVector<ui8>& binarizedFeatures,
                                                        const size_t ignoredFeatureIdx,
                                                        const TFeaturesLayout& layout) {
    TVector<TIndexType> indicesSource = BuildIndicesForBinTree(model, binarizedFeatures, treeId);
    auto samplesCount = indicesSource.size();
    TVector<TVector<TIndexType>> indices(samplesCount, TVector<TIndexType>(1));
    for (int i = 0; i < indicesSource.ysize(); ++i) {
        indices[i][0] = indicesSource[i];
    }

    const int splitCount = model.ObliviousTrees.TreeSizes[treeId];
    auto treeStart = model.ObliviousTrees.TreeStartOffsets[treeId];
    auto& binFeatures = model.ObliviousTrees.GetBinFeatures();
    for (int splitIdx = 0; splitIdx < splitCount; ++splitIdx) {
        const auto& split = binFeatures[model.ObliviousTrees.TreeSplits[treeStart + splitIdx]];
        if (SplitHasFeature(ignoredFeatureIdx, split, layout)) {
            for (size_t doc = 0; doc < samplesCount; ++doc) {
                int indicesCount = indices[doc].ysize();
                for (int i = 0; i < indicesCount; ++i) {
                    indices[doc].push_back(indices[doc][i] ^ (1 << splitIdx));
                }
            }
        }
    }

    return indices;
}

static TVector<TVector<double>> MapFunctionToTrees(
                                            const TFullModel& model,
                                            const TVector<ui8>& binarizedFeatures,
                                            int begin,
                                            int end,
                                            const TTreeFunction& function,
                                            int resultDimension,
                                            const TFeaturesLayout& layout,
                                            int threadCount) {
    if (begin == 0 && end == 0) {
        end = model.ObliviousTrees.TreeSizes.ysize(); //TODO(kirillovs): --model-- add get tree count accessor
    } else {
        end = Min(end, model.ObliviousTrees.TreeSizes.ysize());
    }
    const size_t docCount = binarizedFeatures.size() / model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount();

    TVector<TVector<TVector<double>>> result(CB_THREAD_LIMIT, TVector<TVector<double>>(resultDimension, TVector<double>(docCount)));

    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(threadCount - 1);

    for (int treeBlockIdx = begin; treeBlockIdx < end; treeBlockIdx += CB_THREAD_LIMIT) {
        const int nextBlockIdx = Min(end, treeBlockIdx + CB_THREAD_LIMIT);
        executor.ExecRange(
            [&](int treeIdx) {
                function(model, binarizedFeatures, treeIdx, layout, &result[treeIdx - treeBlockIdx]);
            },
            treeBlockIdx, nextBlockIdx, NPar::TLocalExecutor::WAIT_COMPLETE
        );
    }
    for (int threadIdx = 1; threadIdx < CB_THREAD_LIMIT; ++threadIdx) {
        for (int dim = 0; dim < resultDimension; ++dim) {
            for (size_t doc = 0; doc < docCount; ++doc) {
                result[0][dim][doc] += result[threadIdx][dim][doc];
            }
        }
    }

    return result[0];
}

static bool ModelHasLeafWeightsStats(const TFullModel& model) {
    return !model.ObliviousTrees.LeafWeights.empty();
}

static TVector<TVector<double>> CalcFeatureImportancesForDocuments(const TFullModel& model,
                                                                   const TVector<ui8>& binarizedFeatures,
                                                                   const TVector<TVector<TVector<double>>>& approx,
                                                                   const TFeaturesLayout& layout,
                                                                   int threadCount) {
    const int approxDimension = model.ObliviousTrees.ApproxDimension;
    const int docCount = approx[0][0].ysize();
    const size_t featureCount = model.ObliviousTrees.GetFlatFeatureVectorExpectedSize();

    const TVector<TVector<double>>& leafWeightsStats = model.ObliviousTrees.LeafWeights;
    bool modelHasLeafWeightsStats = ModelHasLeafWeightsStats(model);
    double maxLeafWeight = 0;
    if (modelHasLeafWeightsStats) {
        for (size_t i = 0; i < leafWeightsStats.size(); ++i) {
            float maxLeafWeightForTree = *MaxElement(leafWeightsStats[i].begin(), leafWeightsStats[i].end());
            if (maxLeafWeightForTree > maxLeafWeight) {
                maxLeafWeight = maxLeafWeightForTree;
            }
        }
        CB_ENSURE(maxLeafWeight > 0, "Maximum weight of documents in leaves should be positive.");
    }

    const TTreeFunction CalcFeatureImportanceForTree = [&](const TFullModel& model,
                                                           const TVector<ui8>& binarizedFeatures,
                                                           int treeIdx,
                                                           const TFeaturesLayout& layout,
                                                           TVector<TVector<double>>* resultPtr) { // [featureId][docId]
        TVector<TVector<double>>& result = *resultPtr;
        auto treeFirstLeafPtr = model.ObliviousTrees.GetFirstLeafPtrForTree(treeIdx);
        for (size_t featureId = 0; featureId < featureCount; ++featureId) {
            TVector<TVector<TIndexType>> indices = BuildIndicesWithoutFeature(model,
                                                                              treeIdx,
                                                                              binarizedFeatures,
                                                                              featureId,
                                                                              layout);
            for (int dim = 0; dim < approxDimension; ++dim) {
                for (int doc = 0; doc < docCount; ++doc) {
                    double leafValue = 0, weightedLeafValue = 0, leafWeight = 0, currentLeafWeight;
                    for (int leafIdx = 0; leafIdx < indices[doc].ysize(); ++leafIdx) {
                        if (modelHasLeafWeightsStats) {
                            currentLeafWeight = leafWeightsStats[treeIdx][leafIdx] / maxLeafWeight;
                        } else {
                            currentLeafWeight = 1;
                        }
                        double currentValue = treeFirstLeafPtr[indices[doc][leafIdx] * model.ObliviousTrees.ApproxDimension + dim];
                        leafValue += currentValue;
                        weightedLeafValue += currentLeafWeight * currentValue;
                        leafWeight += currentLeafWeight;
                    }
                    if (leafWeight > 0) {
                        leafValue = weightedLeafValue / leafWeight;
                    } else {
                        leafValue /= static_cast<double>(indices[doc].ysize()); // TODO(bshar): can this be wrong? e.g. when baseline is provided?
                    }
                    result[featureId][doc] += approx[treeIdx][dim][doc] - leafValue;
                }
            }
        }
    };
    return MapFunctionToTrees(model, binarizedFeatures, 0, 0, CalcFeatureImportanceForTree, featureCount, layout, threadCount);
}

static void CalcApproxForTree(const TFullModel& model, const TVector<ui8>& binarizedFeatures,
        size_t treeIdx,
                       TVector<TVector<double>>* resultPtr) {
    TVector<TVector<double>>& approx = *resultPtr;

    const int approxDimension = approx.ysize();
    TVector<TIndexType> indices = BuildIndicesForBinTree(model,
                                               binarizedFeatures,
                                               treeIdx);
    const int docCount = indices.ysize();
    auto treeFirstLeafPtr = model.ObliviousTrees.GetFirstLeafPtrForTree(treeIdx);
    for (int dim = 0; dim < approxDimension; ++dim) {
        for (int doc = 0; doc < docCount; ++doc) {
            approx[dim][doc] += treeFirstLeafPtr[indices[doc] * model.ObliviousTrees.ApproxDimension + dim];
        }
    }
}

TVector<TVector<double>> CalcFeatureImportancesForDocuments(const TFullModel& model,
                                                            const TPool& pool,
                                                            const int threadCount) {
    CB_ENSURE(pool.Docs.GetDocCount() != 0, "Pool should not be empty");
    CB_ENSURE(model.GetTreeCount() != 0, "Model is empty. Did you fit the model?");
    int featureCount = pool.Docs.GetEffectiveFactorCount();
    TFeaturesLayout layout(featureCount, pool.CatFeatures, pool.FeatureId);

    const int approxDimension = model.ObliviousTrees.ApproxDimension;
    const size_t docCount = pool.Docs.GetDocCount();

    auto treeCount = model.ObliviousTrees.GetTreeCount();
    auto binarizedFeatures = BinarizeFeatures(model, pool);
    TVector<TVector<TVector<double>>> approx(treeCount,
                                             TVector<TVector<double>>(approxDimension, TVector<double>(docCount))); // [tree][dim][docIdx]

    for (size_t treeIdx = 0; treeIdx < treeCount; ++treeIdx) {
        CalcApproxForTree(model, binarizedFeatures, treeIdx, &approx[treeIdx]);
    }
    TVector<TVector<double>> result = CalcFeatureImportancesForDocuments(model, binarizedFeatures, approx, layout, threadCount);

    return result;
}
