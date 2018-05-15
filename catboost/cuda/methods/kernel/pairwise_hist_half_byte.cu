#include "pairwise_hist.cuh"
#include "split_properties_helpers.cuh"
#include "compute_pair_hist_loop.cuh"
#include <cooperative_groups.h>
#include <catboost/cuda/cuda_lib/kernel/arch.cuh>
#include <catboost/cuda/cuda_util/kernel/instructions.cuh>
#include <catboost/cuda/cuda_util/kernel/kernel_helpers.cuh>
using namespace cooperative_groups;

namespace NKernel {

    //TODO(noxoomo): tune it
    template<bool IsFullPass>
    struct THalfBytePairwiseHistUnrollTrait {

        static constexpr int InnerUnroll() {
            #if __CUDA_ARCH__ <= 350
            return 2;
            #elif __CUDA_ARCH__ < 700
            return 2;
            #else
            return 8;//IsFullPass ? 4 : 8;
            #endif
        }

        static constexpr int OuterUnroll() {
            #if __CUDA_ARCH__ <= 350
            return 4;
            #elif __CUDA_ARCH__ < 700
            return 2;
            #else
            return 1;
            #endif
        }
    };



    template<int BLOCK_SIZE>
    struct TPairHistHalfByte {
        float* Slice;

        __forceinline__ __device__ int SliceOffset() {
            const int warpOffset = 1024 * (threadIdx.x / 32);
            //we store 4 histograms per block
            // x8 feature and x4 histograms, though histStart = blockIdx * 16
            return warpOffset + (threadIdx.x & 16);
        }


        __forceinline__  __device__ TPairHistHalfByte(float* buff) {
            Slice = buff;
            for (int i = threadIdx.x; i < BLOCK_SIZE * 32; i += BLOCK_SIZE) {
                Slice[i] = 0;
            }
            Slice += SliceOffset();
            __syncthreads();
        }

        __forceinline__ __device__ void AddPair(const ui32 ci1,
                                                const ui32 ci2,
                                                const float w) {

            thread_block_tile<16> groupTile = tiled_partition<16>(this_thread_block());

            const bool flag = threadIdx.x & 1;
            const int shift = threadIdx.x & 14;

            const ui32 bins1 = RotateRight(flag ? ci2 : ci1, 2 * shift);
            const ui32 bins2 = RotateRight(flag ? ci1 : ci2, 2 * shift);

            #pragma unroll
            for (int i = 0; i < 8; i++) {
                const int f = ((shift + 2 * i) & 14);

                const ui32 bin1 = (bins1 >> (28 - 4 * i)) & 15;
                const ui32 bin2 = (bins2 >> (28 - 4 * i)) & 15;

                const int tmp = ((bin1 >= bin2) == flag ? 0 : 512) + f;

                const int offset1 = 32 * bin1 + tmp + flag;
                const int offset2 = 32 * bin2 + tmp + !flag;

                groupTile.sync();
                Slice[offset1] += w;

                groupTile.sync();
                Slice[offset2] += w;
            }
        }

        #if __CUDA_ARCH__ < 700
        template <int N>
        __forceinline__ __device__ void AddPairs(const ui32* ci1,
                                                 const ui32* ci2,
                                                 const float* w) {
            #pragma unroll
            for (int k = 0; k < N; ++k) {
                AddPair(ci1[k], ci2[k], w[k]);
            }
        }
        #else
        template <int N>
        __forceinline__ __device__ void AddPairs(const ui32* ci1,
                                               const ui32* ci2,
                                               const float* w) {

            thread_block_tile<16> groupTile = tiled_partition<16>(this_thread_block());

            const bool flag = threadIdx.x & 1;
            const int shift = threadIdx.x & 14;

            ui32 bins1[N];
            ui32 bins2[N];

            #pragma unroll
            for (int k = 0; k < N; ++k) {
                bins1[k] = RotateRight(flag ? ci2[k] : ci1[k], 2 * shift);
                bins2[k] = RotateRight(flag ? ci1[k] : ci2[k], 2 * shift);
            }

            #pragma unroll
            for (int i = 0; i < 8; i++) {
                const int f = ((shift + 2 * i) & 14);

                int bin1[N];
                int bin2[N];
                #pragma unroll
                for (int k = 0; k < N; ++k) {
                    bin1[k] = (bins1[k] >> (28 - 4 * i)) & 15;
                    bin2[k] = (bins2[k] >> (28 - 4 * i)) & 15;
                }

                int offset1[N];
                int offset2[N];

                #pragma unroll
                for (int k = 0; k < N; ++k) {
                    const int tmp = ((bin1[k] >= bin2[k]) == flag ? 0 : 512) + f;
                    offset1[k] = 32 * bin1[k] + tmp + flag;
                    offset2[k] = 32 * bin2[k] + tmp + !flag;
                }

                groupTile.sync();

                #pragma unroll
                for (int k = 0; k < N; ++k) {
                    Slice[offset1[k]] += w[k];
                }

                groupTile.sync();

                #pragma unroll
                for (int k = 0; k < N; ++k) {
                    Slice[offset2[k]] += w[k];
                }
            }
        }
        #endif



        __forceinline__ __device__  void Reduce() {
            Slice -= SliceOffset();
            __syncthreads();

            {
                const int warpHistSize = 1024;

                for (int start = threadIdx.x; start < warpHistSize; start += BLOCK_SIZE) {
                    float sum = 0;

                    #pragma unroll 12
                    for (int i = start; i < 32 * BLOCK_SIZE; i += warpHistSize) {
                        sum += Slice[i];
                    }

                    Slice[warpHistSize + start] = sum;
                }
            }
            __syncthreads();

            const int maxFoldCount = 16;
            const int fold = (threadIdx.x >> 1) & 15;
            const int f = threadIdx.x / 32;

            if (threadIdx.x < 256) {
                float weightLeq = 0;
                float weightGe = 0;
                const bool isSecondBin = (threadIdx.x & 1);

                if (fold < maxFoldCount) {
                    const volatile float* __restrict__ src = Slice
                                                             + 1024  //warpHistSize
                                                             + 32 * fold
                                                             + 2 * f
                                                             + isSecondBin;

                    weightLeq = src[0] + src[16];
                    weightGe = src[512] + src[528];

                    Slice[4 * (maxFoldCount * f + fold) + isSecondBin] = weightLeq;
                    Slice[4 * (maxFoldCount * f + fold) + 2 + isSecondBin] = weightGe;
                }
            }

            __syncthreads();
        }
    };



    template<int BLOCK_SIZE, int N, int OUTER_UNROLL, int BLOCKS_PER_FEATURE>
    __forceinline__ __device__ void ComputeSplitPropertiesHalfBytePass(const TCFeature* feature, int fCount,
                                                                        const uint* __restrict cindex,
                                                                        const uint2* __restrict pairs, const float* __restrict  weight,
                                                                        const TDataPartition* partition,
                                                                        float* __restrict histogram,
                                                                        float* __restrict smem) {
        using THist = TPairHistHalfByte<BLOCK_SIZE>;
        ComputePairHistogram<BLOCK_SIZE, 1, N, OUTER_UNROLL, BLOCKS_PER_FEATURE,  THist >(partition->Offset, cindex, partition->Size, pairs, weight, smem);


        if (threadIdx.x < 256) {
            const int histId = threadIdx.x & 3;
            const int binId = (threadIdx.x >> 2) & 7;
            const int fid = (threadIdx.x >> 5) & 7;

            if (fid < fCount) {
                const ui32 bfStart = feature[fid].FirstFoldIndex;
                histogram += 4 * bfStart;

                #pragma unroll 2
                for (int fold = binId; fold < feature[fid].Folds; fold += 8) {
                    if (fold < feature[fid].Folds) {
                        const int readOffset = 4 * (16 * fid + fold) + histId;

                        if (BLOCKS_PER_FEATURE > 1) {
                            atomicAdd(histogram + 4 * fold + histId, smem[readOffset]);
                        } else {
                            histogram[4 * fold + histId] += smem[readOffset];
                        }
                    }
                }
            }
        }
    }



    #define DECLARE_PASS_HALF_BYTE(N, OUTER_UNROLL, M) \
        ComputeSplitPropertiesHalfBytePass<BlockSize, N, OUTER_UNROLL, M>(feature, fCount, cindex, pairs, weight, partition, histogram, &localHist[0]);


    template<int BlockSize, bool IsFullPass, int M>
    #if __CUDA_ARCH__ >= 700
    __launch_bounds__(BlockSize, 2)
    #else
    __launch_bounds__(BlockSize, 1)
    #endif
    __global__ void ComputeSplitPropertiesHalfBytePairs(const TCFeature* feature, int fCount, const ui32* cindex,
                                                        const uint2* pairs, const float* weight,
                                                        const TDataPartition* partition,
                                                        int histLineSize,
                                                        float* histogram) {
        //histogram line size - size of one part hist.
        const int featureOffset = (blockIdx.x / M) * 8;
        feature += featureOffset;
        cindex += feature->Offset;
        fCount = min(fCount - featureOffset, 8);

        if (IsFullPass) {
            partition += blockIdx.y;
            histogram += blockIdx.y * histLineSize * 4ULL;
        } else {
            const int depth = (int)log2((float)gridDim.y);
            int partId = GetPairwisePartIdToCalculate(partition);
            partition += partId;
            histogram += (((blockIdx.z + 1) << depth) | blockIdx.y) * histLineSize * 4ULL;
        }

        if (partition->Size == 0) {
            return;
        }

        __shared__ float localHist[32 * BlockSize];

        DECLARE_PASS_HALF_BYTE(THalfBytePairwiseHistUnrollTrait<IsFullPass>::InnerUnroll(), THalfBytePairwiseHistUnrollTrait<IsFullPass>::OuterUnroll(), M)
    }


    void ComputePairwiseHistogramHalfByte(const TCFeature* features,
                                          const ui32 featureCount,
                                          const ui32 halfByteFeatureCount,
                                          const ui32* compressedIndex,
                                          const uint2* pairs, ui32 pairCount,
                                          const float* weight,
                                          const TDataPartition* partition,
                                          ui32 partCount,
                                          ui32 histLineSize,
                                          bool fullPass,
                                          float* histogram,
                                          TCudaStream stream) {
        assert(featureCount == halfByteFeatureCount);
        if (featureCount > 0) {
            const int blockSize = 384;
            dim3 numBlocks;
            numBlocks.x = (featureCount + 7) / 8;
            numBlocks.y = fullPass ? partCount : partCount / 4;
            numBlocks.z = fullPass ? 1 : 3;

            const ui32 blockPerFeatureMultiplier = EstimateBlockPerFeatureMultiplier(numBlocks, pairCount, 64);
            numBlocks.x *= blockPerFeatureMultiplier;


            #define NB_HIST(IS_FULL, BLOCKS_PER_FEATURE)   \
            ComputeSplitPropertiesHalfBytePairs < blockSize, IS_FULL, BLOCKS_PER_FEATURE > << <numBlocks, blockSize, 0, stream>>>(\
                                                  features, featureCount, compressedIndex,  pairs,\
                                                  weight, partition, histLineSize, histogram);

            #define DISPATCH(BLOCKS_PER_FEATURE)  \
            if (fullPass) {                       \
                NB_HIST(true, BLOCKS_PER_FEATURE) \
            } else {                              \
                NB_HIST(false, BLOCKS_PER_FEATURE)\
            }


            if (blockPerFeatureMultiplier == 1) {
                DISPATCH(1);
            } else if (blockPerFeatureMultiplier == 2) {
                DISPATCH(2);
            } else if (blockPerFeatureMultiplier == 4) {
                DISPATCH(4);
            } else if (blockPerFeatureMultiplier == 8) {
                DISPATCH(8);
            } else if (blockPerFeatureMultiplier == 16) {
                DISPATCH(16);
            } else if (blockPerFeatureMultiplier == 32) {
                DISPATCH(32);
            } else if (blockPerFeatureMultiplier == 64) {
                DISPATCH(64);
            } else {
                exit(0);
            }
            #undef NB_HIST
            #undef DISPATCH
        }
    }


}
