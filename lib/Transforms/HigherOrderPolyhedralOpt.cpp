//===- HigherOrderPolyhedralOpt.cpp - High Order Poly Optimization pass -*-===//
//
// Copyright 2019 Uday Bondhugula
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
// =============================================================================
//
// This file implements a matmul optimization sequence based on BLIS. The
// initial step of tiling is left as an exercise. One could start from the
// tiled code to reproduce the remaining steps.
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/Dialect/AffineOps/AffineOps.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/LoopUtils.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#define DEBUG_TYPE "hopt"

using namespace mlir;

static llvm::cl::OptionCategory clOptionsCategory(DEBUG_TYPE " options");

static llvm::cl::opt<bool> clCopy("hopt-copy",
                                  llvm::cl::desc("Explicit copying / packing"),
                                  llvm::cl::init(false),
                                  llvm::cl::cat(clOptionsCategory));

static llvm::cl::opt<bool> clUnroll("hopt-unroll",
                                    llvm::cl::desc("Unroll and unroll-and-jam"),
                                    llvm::cl::init(false),
                                    llvm::cl::cat(clOptionsCategory));

static llvm::cl::opt<bool> clVect("hopt-vect", llvm::cl::desc("Vectorization "),
                                  llvm::cl::init(false),
                                  llvm::cl::cat(clOptionsCategory));

static llvm::cl::opt<bool> clScalRep("hopt-scalrep",
                                     llvm::cl::desc("Scalar Replacement"),
                                     llvm::cl::init(false),
                                     llvm::cl::cat(clOptionsCategory));

namespace {

/// Higher order polyhedral optimization pass.
struct HigherOrderPolyhedralOpt
    : public FunctionPass<HigherOrderPolyhedralOpt> {
  void runOnFunction() override;
  void runOnBlock(Block *block);

  void lowerPolyForOps(Block *block, Block::iterator begin, Block::iterator end,
                       OpBuilder &builder);

  static void optimizeMatmul(AffineForOp rootMatmulNest, unsigned M_C,
                             unsigned N_C, unsigned K_C, unsigned M_R,
                             unsigned N_R, unsigned K_U, OpBuilder &builder);

  static const llvm::DenseMap<StringRef, unsigned> optConf;

  static const char *kPolyCodeGenAttrName;
};

} // end anonymous namespace

const llvm::DenseMap<StringRef, unsigned> HigherOrderPolyhedralOpt::optConf = {
    {"M_C", 64}, {"N_C", 128}, {"K_C", 512},
    {"M_R", 4},  {"N_R", 4},   {"K_U", 4}};

const char *HigherOrderPolyhedralOpt::kPolyCodeGenAttrName =
    "poly_codegen_name";

/// Creates a pass to perform optimizations relying on memref dataflow such as
/// store to load forwarding, elimination of dead stores, and dead allocs.
std::unique_ptr<OpPassBase<FuncOp>> mlir::createHigherOrderPolyhedralOptPass() {
  return std::make_unique<HigherOrderPolyhedralOpt>();
}

static unsigned getMatmulOptParameter(Operation *op, StringRef name) {
  IntegerAttr attr = op->getAttrOfType<IntegerAttr>(name);
  if (!attr) {
    // Use the default value.
    assert(HigherOrderPolyhedralOpt::optConf.count(name) > 0 &&
           "default opt conf parameter not found");
    return HigherOrderPolyhedralOpt::optConf.lookup(name);
  }
  return attr.getValue().getSExtValue();
}

static AffineForOp getByPolyName(AffineForOp root, StringRef polyName) {
  AffineForOp res;

  root.walk([&](AffineForOp forOp) {
    auto stringAttr = forOp.getAttrOfType<StringAttr>(
        HigherOrderPolyhedralOpt::kPolyCodeGenAttrName);
    if (!stringAttr)
      return WalkResult::advance();
    auto forOpCodegenName = stringAttr.getValue();
    if (forOpCodegenName.equals(polyName)) {
      res = forOp;
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return res;
}

/// Optimize matmul nest with vectorization, packing, and register tiling.
void HigherOrderPolyhedralOpt::optimizeMatmul(AffineForOp rootMatmulNest,
                                              unsigned M_C, unsigned N_C,
                                              unsigned K_C, unsigned M_R,
                                              unsigned N_R, unsigned K_U,
                                              OpBuilder &builder) {
  Value *outputMemRef = NULL, *lhsMemRef = NULL, *rhsMemRef = NULL;
  // Identify the LHS, RHS, and output memrefs.
  rootMatmulNest.walk(
      [&](AffineStoreOp storeOp) { outputMemRef = storeOp.getMemRef(); });
  rootMatmulNest.walk([&](AffineLoadOp loadOp) {
    if (outputMemRef == loadOp.getMemRef())
      return;
    rhsMemRef = loadOp.getMemRef();
  });
  rootMatmulNest.walk([&](AffineLoadOp loadOp) {
    if (rhsMemRef == loadOp.getMemRef() || outputMemRef == loadOp.getMemRef())
      return;
    lhsMemRef = loadOp.getMemRef();
  });

  assert(outputMemRef && lhsMemRef && rhsMemRef &&
         "unable to identify memrefs");
  AffineForOp jC = getByPolyName(rootMatmulNest, "jC");
  (void)jC;
  // It is fine if jC or kC are not found (due to large tile sizes).
  AffineForOp kC = getByPolyName(rootMatmulNest, "kC");
  (void)kC;

  AffineForOp iC = getByPolyName(rootMatmulNest, "iC");
  if (!iC) {
    LLVM_DEBUG(llvm::dbgs()
               << "BLIS transformation recipe failed: iC not found\n");
    return;
  }

  AffineForOp jR = getByPolyName(rootMatmulNest, "jR");
  if (!jR) {
    LLVM_DEBUG(llvm::dbgs()
               << "BLIS transformation recipe failed: jR not found\n");
    return;
  }

  AffineForOp k = getByPolyName(rootMatmulNest, "k");
  AffineForOp jjR = getByPolyName(rootMatmulNest, "jjR");
  AffineForOp iiR = getByPolyName(rootMatmulNest, "iiR");
  // It is fine if iiR, jjR are not found (due to degenerate tile sizes).

  if (clVect && jjR &&
      !outputMemRef->getType()
           .cast<MemRefType>()
           .getElementType()
           .isa<VectorType>()) {
    DenseMap<Value *, Value *> vecMemRefMap;
    if (succeeded(loopVectorize(jjR, &vecMemRefMap))) {
      assert(vecMemRefMap.count(rhsMemRef) > 0 && "rhs vec memref not found");
      assert(vecMemRefMap.count(outputMemRef) > 0 &&
             "output vec memref not found");

      rhsMemRef = vecMemRefMap[rhsMemRef];
      outputMemRef = vecMemRefMap[outputMemRef];
    }
  }

  Value *lhsBuf, *rhsL3Buf, *rhsL1Buf;

  // Packing.
  if (clCopy) {
    AffineCopyOptions copyOptions = {/*generateDma=*/false,
                                     /*slowMemorySpace=*/0,
                                     /*fastMemorySpace=*/0,
                                     /*tagMemorySpace=*/0,
                                     /*fastMemCapacityBytes=*/2 * 1024 * 1024UL,
                                     AffineMap()};

    // For the LHS matrix (pack into L2).
    auto d0 = builder.getAffineDimExpr(0);
    auto d1 = builder.getAffineDimExpr(1);
    SmallVector<AffineExpr, 4> bufRemapExprs = {d0.floorDiv(M_R), d1, d0 % M_R};
    copyOptions.fastBufferLayout = AffineMap();
    SmallVector<Value *, 1> fastBuf;
    DenseSet<Operation *> copyNests;
    affineDataCopyGenerate(iC.getBody()->begin(),
                           std::prev(iC.getBody()->end()), copyOptions,
                           lhsMemRef, copyNests, &fastBuf);
    lhsBuf = fastBuf[0];

    if (kC) {
      // RHS matrix, pack into L3 tile if the kC loop exists.
      copyOptions.fastBufferLayout = AffineMap();
      affineDataCopyGenerate(kC.getBody()->begin(),
                             std::prev(kC.getBody()->end()), copyOptions,
                             rhsMemRef, copyNests, &fastBuf);
      rhsL3Buf = fastBuf[0];
    } else {
      rhsL3Buf = rhsMemRef;
    }

    // For the RHS matrix (pack into L1).
    copyOptions.fastBufferLayout = AffineMap();
    copyOptions.fastMemCapacityBytes = 256 * 1024UL;
    affineDataCopyGenerate(jR.getBody()->begin(),
                           std::prev(jR.getBody()->end()), copyOptions,
                           /*filterMemRef=*/rhsL3Buf, copyNests, &fastBuf);
    rhsL1Buf = fastBuf[0];

    // Set alignment to 256-bit boundaries for LHS and RHS buffers.
    // FIXME: you don't need to set alignment if these are already vector
    // memrefs.
    lhsBuf->getDefiningOp()->setAttr("alignment",
                                     builder.getI64IntegerAttr(32));
    // The rhsL3buf could sometimes just be the original memref / func arg.
    if (auto rhsAllocOp = rhsL3Buf->getDefiningOp())
      rhsAllocOp->setAttr("alignment", builder.getI64IntegerAttr(32));
    rhsL1Buf->getDefiningOp()->setAttr("alignment",
                                       builder.getI64IntegerAttr(32));
  }

  if (clUnroll) {
    // Unroll the intra register tile loops fully. We are going to identity
    // loops based on the attributes set when converting the IST AST. These
    // are reliable instead of depth-based matching in MLIR since loops are
    // subject to disappearing depending on the tile sizes and constant
    // problem sizes.
    if (iiR)
      loopUnrollJamUpToFactor(iiR, M_R);
    if (jjR)
      loopUnrollJamUpToFactor(jjR, N_R);
    if (k)
      loopUnrollJamByFactor(k, K_U);
  }
}

void HigherOrderPolyhedralOpt::runOnBlock(Block *block) {
  for (auto &op : *block) {
    if (auto forOp = dyn_cast<AffineForOp>(op)) {
      StringAttr polyClass =
          forOp.getOperation()->getAttrOfType<StringAttr>("class");
      if (!polyClass || !polyClass.getValue().equals("matmul"))
        continue;

      OpBuilder builder(forOp);

      auto M_C = getMatmulOptParameter(forOp, "M_C");
      auto N_C = getMatmulOptParameter(forOp, "N_C");
      auto K_C = getMatmulOptParameter(forOp, "K_C");
      auto M_R = getMatmulOptParameter(forOp, "M_R");
      auto N_R = getMatmulOptParameter(forOp, "N_R");
      auto K_U = getMatmulOptParameter(forOp, "K_U");

      optimizeMatmul(forOp, M_C, N_C, K_C, M_R, N_R, K_U, builder);
    }
  }
}

void HigherOrderPolyhedralOpt::runOnFunction() {
  auto func = getFunction();

  // Process all blocks of the function.
  for (auto &block : func)
    runOnBlock(&block);

  // Normalize non-identity layouts used.
  func.walk([](AllocOp allocOp) { normalizeMemRef(allocOp); });

  // Canonicalize.
  {
    OwningRewritePatternList patterns;
    auto *context = &getContext();
    AffineLoadOp::getCanonicalizationPatterns(patterns, context);
    AffineStoreOp::getCanonicalizationPatterns(patterns, context);
    AffineApplyOp::getCanonicalizationPatterns(patterns, context);
    applyPatternsGreedily(func, patterns);
  }

  // Replace accesses to invariant load/store's and multiple redundant loads
  // by scalars.
  if (clScalRep) {
    func.walk([&](AffineForOp forOp) { scalarReplace(forOp); });
  }
}

static PassRegistration<HigherOrderPolyhedralOpt>
    pass("hopt", "Run higher order polyhedral optimization");
