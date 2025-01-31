// Copyright 2022 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sort.h"

#include "absl/numeric/bits.h"
#include "emp-tool/circuits/number.h"

#include "spu/kernel/hal/permute_util.h"
#include "spu/kernel/hal/polymorphic.h"
#include "spu/kernel/hlo/casting.h"
#include "spu/kernel/hlo/utils.h"

namespace spu::kernel::hlo {
namespace {

void sliceCopy(spu::Value &dst, const spu::Value &src,
               std::vector<int64_t> dst_indices, size_t dim) {
  auto copy_size = src.shape()[0];
  for (int64_t idx = 0; idx < copy_size; ++idx) {
    dst_indices[dim] = idx;
    dst.copyElementFrom(src, {idx}, dst_indices);
  }
}

std::vector<spu::Value> getValuesToSort(HalContext *ctx,
                                        absl::Span<const spu::Value> inputs,
                                        const std::vector<int64_t> &indices,
                                        int64_t sort_dim,
                                        int64_t sort_dim_elements,
                                        int64_t num_operands) {
  std::vector<int64_t> limit_indices(indices.begin(), indices.end());
  std::for_each(limit_indices.begin(), limit_indices.end(),
                [](int64_t &index) { ++index; });
  limit_indices[sort_dim] = sort_dim_elements;
  std::vector<spu::Value> values_to_sort;
  values_to_sort.reserve(num_operands);
  for (int64_t i = 0; i < num_operands; ++i) {
    auto value_to_sort = hal::reshape(
        ctx, hal::slice(ctx, inputs[i], indices, limit_indices, {}),
        {sort_dim_elements});
    values_to_sort.push_back(std::move(value_to_sort));
  }
  return values_to_sort;
}

// Refers to implementation of emp-tool:
// https://github.com/emp-toolkit/emp-tool/blob/b07a7d9ab3053a3e16991751402742d418377f63/emp-tool/circuits/number.h
void cmpSwap(HalContext *ctx, const CompFn &comparator_body,
             std::vector<spu::Value> *values_to_sort, int64_t x_start_indices,
             int64_t y_start_indices, int64_t n, bool acc) {
  size_t num_operands = values_to_sort->size();

  std::vector<spu::Value> values;
  values.reserve(2 * num_operands);
  for (size_t i = 0; i < num_operands; ++i) {
    values.push_back(hal::slice(ctx, values_to_sort->at(i), {x_start_indices},
                                {x_start_indices + n}, {1}));
    values.push_back(hal::slice(ctx, values_to_sort->at(i), {y_start_indices},
                                {y_start_indices + n}, {1}));
  }
  spu::Value predicate = comparator_body(values);

  for (size_t i = 0; i < num_operands; ++i) {
    auto fst = hal::slice(ctx, values_to_sort->at(i), {x_start_indices},
                          {x_start_indices + n}, {1});
    auto snd = hal::slice(ctx, values_to_sort->at(i), {y_start_indices},
                          {y_start_indices + n}, {1});

    auto greater = spu::kernel::hal::select(ctx, predicate, fst, snd);
    auto less = spu::kernel::hal::select(ctx, predicate, snd, fst);

    if (acc) {
      values_to_sort->at(i).copyElementFrom(
          greater, {}, {static_cast<int64_t>(x_start_indices)},
          static_cast<int64_t>(n * values_to_sort->front().elsize()));
      values_to_sort->at(i).copyElementFrom(
          less, {}, {static_cast<int64_t>(y_start_indices)},
          static_cast<int64_t>(n * values_to_sort->front().elsize()));
    } else {
      values_to_sort->at(i).copyElementFrom(
          less, {}, {static_cast<int64_t>(x_start_indices)},
          static_cast<int64_t>(n * values_to_sort->front().elsize()));
      values_to_sort->at(i).copyElementFrom(
          greater, {}, {static_cast<int64_t>(y_start_indices)},
          static_cast<int64_t>(n * values_to_sort->front().elsize()));
    }
  }
}

void sequentialBitonicMerge(HalContext *ctx, const CompFn &comparator_body,
                            std::vector<spu::Value> *values_to_sort, size_t lo,
                            size_t n, bool acc) {
  if (n > 1) {
    size_t m = emp::greatestPowerOfTwoLessThan(n);
    cmpSwap(ctx, comparator_body, values_to_sort, lo, lo + m, n - m, acc);

    sequentialBitonicMerge(ctx, comparator_body, values_to_sort, lo, m, acc);
    sequentialBitonicMerge(ctx, comparator_body, values_to_sort, lo + m, n - m,
                           acc);
  }
}

void sequentialBitonicSort(HalContext *ctx, const CompFn &comparator_body,
                           std::vector<spu::Value> *values_to_sort, size_t lo,
                           size_t n, bool acc) {
  if (n > 1) {
    size_t m = (n >> 1);
    sequentialBitonicSort(ctx, comparator_body, values_to_sort, lo, m, !acc);
    sequentialBitonicSort(ctx, comparator_body, values_to_sort, lo + m, n - m,
                          acc);
    sequentialBitonicMerge(ctx, comparator_body, values_to_sort, lo, n, acc);
  }
}

void generateBitonicSortIndex(size_t n,
                              std::vector<std::vector<size_t>> *indices) {
  YACL_ENFORCE(absl::has_single_bit(n));
  size_t stage = absl::bit_width(n) - 1;

  for (int i = static_cast<int>(stage); i > 0; i--) {
    std::vector<size_t> fst;
    std::vector<size_t> sec;

    for (size_t j = 0; j < n; j++) {
      if (((j >> (i - 1)) & 1) == 0) {
        fst.emplace_back(j);
      } else {
        sec.emplace_back(j);
      }
    }

    fst.insert(fst.end(), sec.begin(), sec.end());
    indices->emplace_back(fst);
  }
}

void generateBitonicMergeIndex(size_t n,
                               std::vector<std::vector<size_t>> *indices) {
  YACL_ENFORCE(absl::has_single_bit(n));
  size_t stage = absl::bit_width(n) - 1;

  for (int stage_idx = 0; stage_idx < static_cast<int>(stage - 1);
       stage_idx++) {
    for (int substage_idx = static_cast<int>(stage_idx); substage_idx > -1;
         substage_idx--) {
      std::vector<size_t> fst;
      std::vector<size_t> sec;
      for (size_t i = 0; i < n; i++) {
        bool asc_flag = ((i >> (stage_idx + 1)) & 1) == 0;
        bool fst_flag = ((i >> substage_idx) & 1) == 0;

        if (asc_flag ^ fst_flag) {
          sec.emplace_back(i);
        } else {
          fst.emplace_back(i);
        }
      }

      fst.insert(fst.end(), sec.begin(), sec.end());
      indices->emplace_back(fst);
    }
  }
}

std::vector<spu::Value> parallelBitonicSort(
    HalContext *ctx, const CompFn &comparator_body,
    const std::vector<spu::Value> &values_to_sort, size_t n) {
  YACL_ENFORCE(absl::has_single_bit(n));

  std::vector<std::vector<size_t>> indices;
  generateBitonicMergeIndex(n, &indices);
  generateBitonicSortIndex(n, &indices);

  std::vector<spu::Value> target = values_to_sort;

  for (const auto &index : indices) {
    // permute
    std::vector<spu::Value> permuted_values;

    for (auto v : target) {
      permuted_values.emplace_back(hal::permute(ctx, v, 0, xt::adapt(index)));
    }

    // cmp and swap
    cmpSwap(ctx, comparator_body, &permuted_values, 0,
            static_cast<int64_t>(n / 2), static_cast<int64_t>(n / 2), true);

    // inverse permute
    std::vector<size_t> inverse_permutation(index.size());
    std::iota(inverse_permutation.begin(), inverse_permutation.end(), 0);
    std::sort(inverse_permutation.begin(), inverse_permutation.end(),
              [&index](int left, int right) -> bool {
                return index[left] < index[right];
              });

    target.clear();

    for (auto v : permuted_values) {
      target.emplace_back(
          hal::permute(ctx, v, 0, xt::adapt(inverse_permutation)));
    }
  }

  return target;
}

}  // namespace

std::vector<spu::Value> Sort(HalContext *ctx,
                             absl::Span<const spu::Value> inputs,
                             int64_t sort_dim, bool is_stable,
                             const CompFn &comparator_body,
                             Visibility comparator_ret_vis) {
  int64_t num_operands = inputs.size();
  auto key_shape = inputs[0].shape();
  auto rank = key_shape.size();
  std::vector<spu::Value> results;
  results.reserve(num_operands);
  for (int64_t i = 0; i < num_operands; ++i) {
    results.emplace_back(
        NdArrayRef(inputs[i].data().eltype(), inputs[i].shape()),
        inputs[i].dtype());
  }
  std::vector<int64_t> zero_base(rank, 0);
  std::vector<int64_t> increment(rank, 1);
  int64_t sort_dim_elements = key_shape[sort_dim];
  YACL_ENFORCE(
      sort_dim >= 0 && sort_dim < static_cast<int64_t>(increment.size()),
      "Unexpected out-of-bound sort dimension {}"
      " accessing increment of size {} ",
      sort_dim, increment.size());
  increment[sort_dim] = sort_dim_elements;

  if (comparator_ret_vis == VIS_PUBLIC) {
    // Iterate through each dimension except 'sort_dim'.
    forEachIndex(key_shape, zero_base, key_shape, increment,
                 [&](const std::vector<int64_t> &indices) {
                   // Extract a slice from each operand literal that corresponds
                   // to exactly the row in dimension 'sort_dim'.
                   std::vector<spu::Value> values_to_sort =
                       getValuesToSort(ctx, inputs, indices, sort_dim,
                                       sort_dim_elements, num_operands);

                   std::vector<int64_t> indices_to_sort(sort_dim_elements);
                   std::iota(indices_to_sort.begin(), indices_to_sort.end(), 0);
                   auto comparator = [&comparator_body, &num_operands, &ctx,
                                      &values_to_sort](int64_t a, int64_t b) {
                     std::vector<spu::Value> values;
                     values.reserve(2 * num_operands);
                     for (int64_t i = 0; i < num_operands; ++i) {
                       values.push_back(values_to_sort[i].getElementAt(a));
                       values.push_back(values_to_sort[i].getElementAt(b));
                     }
                     spu::Value ret = comparator_body(values);
                     return getConditionValue(ctx, ret);
                   };

                   if (is_stable) {
                     std::stable_sort(indices_to_sort.begin(),
                                      indices_to_sort.end(), comparator);
                   } else {
                     std::sort(indices_to_sort.begin(), indices_to_sort.end(),
                               comparator);
                   }

                   std::vector<int64_t> start_indices(rank, 0);
                   for (int64_t i = 0; i < num_operands; ++i) {
                     auto sorted_value = hal::permute(
                         ctx, values_to_sort[i], 0, xt::adapt(indices_to_sort));
                     sliceCopy(results[i], sorted_value, indices, sort_dim);
                   }
                 });
  } else {
    // Iterate through each dimension except 'sort_dim'.
    forEachIndex(
        key_shape, zero_base, key_shape, increment,
        [&](const std::vector<int64_t> &indices) {
          std::vector<spu::Value> values_to_sort = getValuesToSort(
              ctx, inputs, indices, sort_dim, sort_dim_elements, num_operands);

          int64_t n = values_to_sort[0].numel();
          if (absl::has_single_bit(static_cast<size_t>(n))) {
            // TODO(junfeng): add paddings when n x is not an integral power of
            // two.
            values_to_sort =
                parallelBitonicSort(ctx, comparator_body, values_to_sort, n);
          } else {
            sequentialBitonicSort(ctx, comparator_body, &values_to_sort, 0, n,
                                  true);
          }

          for (int64_t i = 0; i < num_operands; ++i) {
            sliceCopy(results[i], values_to_sort[i], indices, sort_dim);
          }
        });
  }

  return results;
}

}  // namespace spu::kernel::hlo
