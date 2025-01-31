// Copyright 2021 Ant Group Co., Ltd.
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

#include "spu/device/executor.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Value.h"
#include "yacl/base/exception.h"

#include "spu/kernel/context.h"
#include "spu/kernel/value.h"

namespace spu::device {

const spu::Value &SymbolScope::lookupValue(mlir::Value key) const {
  auto itr = symbols_.find(key);

  if (itr != symbols_.end()) {
    return itr->second;
  }

  if (parent_ != nullptr) {
    return parent_->lookupValue(key);
  }

  // Somehow cannot find this value on stack, print a reasonable error
  YACL_THROW("TODO: add more details");
  // YACL_THROW("Try to get a non-exist value, defined at {} ",
  //            mlirObjectToString(*v.getDefiningOp()));
}

void SymbolScope::addValue(mlir::Value key, const spu::Value &val) {
  symbols_[key] = val;
}

void SymbolScope::addValue(mlir::Value key, spu::Value &&val) {
  symbols_[key] = std::move(val);
}

std::vector<spu::Value> runRegion(OpExecutor *executor,                //
                                  HalContext *hctx,                    //
                                  SymbolScope *parent_scope,           //
                                  mlir::Region &region,                //
                                  absl::Span<spu::Value const> params, //
                                  const ExecutionOptions &opts) {
  YACL_ENFORCE(region.getNumArguments() == params.size(),
               "region requires {} arguments while got number of params {}",
               region.getRegionNumber(), params.size());

  // create a new scope for this region.
  SymbolScope sscope(parent_scope);

  // inject the parameters to region's symbol table.
  for (const auto &blkarg : region.getArguments()) {
    sscope.addValue(blkarg, params[blkarg.getArgNumber()]);
  }

  YACL_ENFORCE(region.hasOneBlock());
  return runBlock(executor, hctx, &sscope, region.front(), params, opts);
}

std::vector<spu::Value> runBlock(OpExecutor *executor, HalContext *hctx,
                                 SymbolScope *symbols, mlir::Block &block,
                                 absl::Span<spu::Value const> params,
                                 const ExecutionOptions &opts) {
  for (auto &op : block.without_terminator()) {
    executor->runKernel(hctx, symbols, op);
  }

  if (auto *termOp = block.getTerminator()) {
    // TODO: enforce ReturnLike
    std::vector<spu::Value> results;
    results.reserve(termOp->getNumOperands());
    for (const auto operand : termOp->getOperands()) {
      results.emplace_back(symbols->lookupValue(operand));
    }
    return results;
  }

  // No terminator
  YACL_THROW("Should not be here");
}

std::vector<spu::Value> runBlockParallel(OpExecutor *executor, HalContext *hctx,
                                         SymbolScope *symbols,
                                         mlir::Block &block,
                                         absl::Span<spu::Value const> params,
                                         const ExecutionOptions &opts) {
  YACL_THROW("TODO");
}

} // namespace spu::device
