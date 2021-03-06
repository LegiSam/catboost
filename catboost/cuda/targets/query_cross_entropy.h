#pragma once

#include "target_func.h"
#include "non_diag_target_der.h"
#include "non_diagonal_oralce_type.h"

#include <catboost/libs/options/enums.h>
#include <catboost/libs/options/loss_description.h>
#include <catboost/cuda/gpu_data/dataset_base.h>
#include <catboost/cuda/gpu_data/querywise_helper.h>
#include <catboost/cuda/cuda_util/sort.h>
#include <catboost/cuda/methods/helpers.h>
#include <catboost/libs/options/bootstrap_options.h>

namespace NCatboostCuda {
    template <class TMapping>
    class TQueryCrossEntropy;

    template <>
    class TQueryCrossEntropy<NCudaLib::TStripeMapping> : public TNonDiagQuerywiseTarget<NCudaLib::TStripeMapping> {
    public:
        using TSamplesMapping = NCudaLib::TStripeMapping;
        using TParent = TNonDiagQuerywiseTarget<TSamplesMapping>;
        using TStat = TAdditiveStatistic;
        using TMapping = TSamplesMapping;
        CB_DEFINE_CUDA_TARGET_BUFFERS();

        template <class TDataSet>
        TQueryCrossEntropy(const TDataSet& dataSet,
                           TGpuAwareRandom& random,
                           const NCatboostOptions::TLossDescription& targetOptions)
            : TParent(dataSet,
                      random) {
            Init(targetOptions);
        }

        TQueryCrossEntropy(TQueryCrossEntropy&& other)
            : TParent(std::move(other))
            , Alpha(other.Alpha)
        {
        }

        using TParent::GetTarget;

        TAdditiveStatistic ComputeStats(const TConstVec& point, double alpha) const;

        TAdditiveStatistic ComputeStats(const TConstVec& point) const {
            return ComputeStats(point, Alpha);
        }

        TAdditiveStatistic ComputeStats(const TConstVec& point,
                                        const TMap<TString, TString>& params) const {
            return ComputeStats(point, NCatboostOptions::GetAlphaQueryCrossEntropy(params));
        }

        static double Score(const TAdditiveStatistic& score) {
            return -score.Stats[0] / score.Stats[1];
        }

        double Score(const TConstVec& point) const {
            return Score(ComputeStats(point));
        }

        void StochasticGradient(const TConstVec&,
                                const NCatboostOptions::TBootstrapConfig&,
                                TNonDiagQuerywiseTargetDers*) const {
            CB_ENSURE(false, "Stochastic gradient is useless for LLMax");
        }

        void StochasticNewton(const TConstVec& point,
                              const NCatboostOptions::TBootstrapConfig& config,
                              TNonDiagQuerywiseTargetDers* target) const {
            ApproximateStochastic(point, config, target);
        }

        void ApproximateStochastic(const TConstVec& point,
                                   const NCatboostOptions::TBootstrapConfig& bootstrapConfig,
                                   TNonDiagQuerywiseTargetDers* target) const;

        void CreateSecondDerMatrix(NCudaLib::TCudaBuffer<uint2, NCudaLib::TStripeMapping>* pairs) const;

        TStripeBuffer<const ui32> GetApproximateQids() const {
            const auto& cachedData = GetCachedMetadata();
            return cachedData.FuncValueQids.ConstCopyView();
        }

        TStripeBuffer<const float> GetApproximateOrderWeights() const {
            const auto& cachedData = GetCachedMetadata();
            return cachedData.FuncValueWeights.ConstCopyView();
        }

        TStripeBuffer<const ui32> GetApproximateQidOffsets() const {
            const auto& cachedData = GetCachedMetadata();
            return cachedData.FuncValueQidOffsets.ConstCopyView();
        }

        TStripeBuffer<const ui32> GetApproximateDocOrder() const {
            const auto& cachedData = GetCachedMetadata();
            return cachedData.FuncValueOrder;
        }

        void ApproximateAt(const TConstVec& orderedPoint,
                           TStripeBuffer<float>* score,
                           TStripeBuffer<float>* der,
                           TStripeBuffer<float>* pointDer2,
                           TStripeBuffer<float>* groupDer2,
                           TStripeBuffer<float>* groupSumDer2) const;
        static constexpr bool IsMinOptimal() {
            return true;
        }

        TStringBuf ScoreMetricName() {
            return TStringBuilder() << "QueryCrossEntropy:alpha=" << Alpha;
        }

        ELossFunction GetScoreMetricType() const {
            return ELossFunction::QueryCrossEntropy;
        }

        static constexpr ENonDiagonalOracleType NonDiagonalOracleType() {
            return ENonDiagonalOracleType::Groupwise;
        }

    private:
        struct TQueryLogitApproxHelpData {
            TCudaBuffer<float, TMapping> FuncValueTarget;
            TCudaBuffer<float, TMapping> FuncValueWeights;
            TCudaBuffer<ui32, TMapping> FuncValueOrder;
            TCudaBuffer<bool, TMapping> FuncValueFlags;
            TCudaBuffer<ui32, TMapping> FuncValueQids;
            TCudaBuffer<ui32, TMapping> FuncValueQidOffsets;
        };

    private:
        ui32 GetMaxQuerySize() const {
            return 256;
        }

        bool HasBigQueries() const {
            return false;
        }

        void Init(const NCatboostOptions::TLossDescription& targetOptions) {
            CB_ENSURE(targetOptions.GetLossFunction() == ELossFunction::QueryCrossEntropy);
            Alpha = NCatboostOptions::GetAlphaQueryCrossEntropy(targetOptions);
        }

        TQuerywiseSampler& GetQueriesSampler() const {
            if (QueriesSampler == nullptr) {
                QueriesSampler = new TQuerywiseSampler();
            }
            return *QueriesSampler;
        }

        void MakeQidsForLLMax(TStripeBuffer<ui32>* order,
                              TStripeBuffer<ui32>* orderQids,
                              TStripeBuffer<ui32>* orderQidOffsets,
                              TStripeBuffer<bool>* flags) const;

        const TQueryLogitApproxHelpData& GetCachedMetadata() const;

        double GetMeanQuerySize() const {
            double totalDocs = GetTarget().GetTargets().GetObjectsSlice().Size();
            double totalQueries = TParent::GetSamplesGrouping().GetQueryCount();
            return totalQueries > 0 ? totalDocs / totalQueries : 0;
        }

    private:
        mutable THolder<TQuerywiseSampler> QueriesSampler;
        double Alpha;
        mutable TQueryLogitApproxHelpData CachedMetadata;
    };



}
