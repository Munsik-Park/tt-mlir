// SPDX-FileCopyrightText: (c) 2025 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ttmlir/Conversion/TTIRToTTKernel/TTIRToTTKernel.h"

#include "ttmlir/Dialect/TTIR/IR/TTIRGenericRegionOps.h"
#include "ttmlir/Dialect/TTIR/IR/TTIROpsInterfaces.h"
#include "ttmlir/Dialect/TTIR/IR/TTIRTraits.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernelOps.h"
#include "ttmlir/Utils.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir::tt::ttkernel {

namespace {

static Value i32(OpBuilder &rewriter, Location loc, int32_t value) {
  return rewriter
      .create<arith::ConstantOp>(loc, rewriter.getI32Type(),
                                 rewriter.getI32IntegerAttr(value))
      .getResult();
}

static Value index(OpBuilder &rewriter, Location loc, int64_t value) {
  return rewriter
      .create<arith::ConstantOp>(loc, rewriter.getIndexType(),
                                 rewriter.getIndexAttr(value))
      .getResult();
}

static std::pair<Value, Value>
getVirtualCoordsFromLogicalCoords(PatternRewriter &rewriter, Location loc,
                                  ChipDescAttr chipDesc,
                                  ValueRange dstCoreIndex) {
  auto offset = chipDesc.getCoordTranslationOffsets();

  return {rewriter
              .create<arith::AddIOp>(dstCoreIndex[0].getLoc(), dstCoreIndex[0],
                                     index(rewriter, loc, offset[0]))
              .getResult(),
          rewriter
              .create<arith::AddIOp>(dstCoreIndex[1].getLoc(), dstCoreIndex[1],
                                     index(rewriter, loc, offset[1]))
              .getResult()};
}

static std::pair<Value, Value> getMcastEndCoords(PatternRewriter &rewriter,
                                                 Location loc, Value &nocStartY,
                                                 Value &nocStartX,
                                                 OperandRange mcastShape) {
  return {rewriter.create<arith::SubIOp>(
              nocStartY.getLoc(),
              rewriter.create<arith::AddIOp>(nocStartY.getLoc(), nocStartY,
                                             mcastShape[0]),
              index(rewriter, loc, 1)),
          rewriter.create<arith::SubIOp>(
              nocStartX.getLoc(),
              rewriter.create<arith::AddIOp>(nocStartX.getLoc(), nocStartX,
                                             mcastShape[1]),
              index(rewriter, loc, 1))};
}

static Value getCB(ConversionPatternRewriter &rewriter, Value cb) {
  memref::LoadOp loadOp = mlir::cast<memref::LoadOp>(cb.getDefiningOp());
  assert(loadOp.getIndices().size() == 1 &&
         "Expected single index in load op, failing.");
  return rewriter.getRemappedValue(loadOp.getMemref());
}

static Value getLoadIndex(Value tile) {
  memref::LoadOp loadOp = mlir::cast<memref::LoadOp>(tile.getDefiningOp());
  assert(loadOp.getIndices().size() == 1 &&
         "Expected single index in load op, failing.");
  return loadOp.getIndices().front();
}

static void lowerLoadToCopyTile(memref::LoadOp op, bool cbIdxAsDstIdx,
                                ConversionPatternRewriter &rewriter) {
  auto index = [&](int64_t value) {
    return rewriter
        .create<arith::ConstantOp>(op.getLoc(), rewriter.getIndexType(),
                                   rewriter.getIndexAttr(value))
        .getResult();
  };

  auto cb = rewriter.getRemappedValue(op.getMemref());
  auto cbType = mlir::cast<ttkernel::CBType>(cb.getType());
  rewriter.create<ttkernel::CopyTileInitOp>(op.getLoc(), cb);
  rewriter.create<ttkernel::CopyTileOp>(
      op.getLoc(), cb, op.getIndices().front(),
      cbIdxAsDstIdx ? index(static_cast<uint32_t>(cbType.getPort()))
                    : index(0));
}

static void setInsertionPointAfterOperands(OpBuilder &rewriter,
                                           llvm::ArrayRef<Value> operands) {
  Operation *latestDefOp = nullptr;
  for (Value operand : operands) {
    Operation *definingOp = operand.getDefiningOp();
    if (!latestDefOp ||
        (definingOp && !definingOp->isBeforeInBlock(latestDefOp))) {
      latestDefOp = definingOp;
    }
  }

  rewriter.setInsertionPointAfter(latestDefOp);
}

} // namespace

namespace {

class MemrefStoreRewriter : public OpConversionPattern<memref::StoreOp> {
public:
  using OpConversionPattern<memref::StoreOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(memref::StoreOp op, memref::StoreOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto cb = adaptor.getMemref();
    auto storeIdx = op.getIndices().front();
    rewriter.replaceOpWithNewOp<ttkernel::PackTileOp>(
        op, index(rewriter, op->getLoc(), 0), cb, storeIdx,
        rewriter.getBoolAttr(true));

    return success();
  };
};

} // namespace

namespace {

template <typename ConcreteOp, typename InitOp, typename FPUOp>
class TTIRFPUOpsRewriter : public OpConversionPattern<ConcreteOp> {
public:
  using OpConversionPattern<ConcreteOp>::OpConversionPattern;
  static_assert(FPUOp::template hasTrait<TTKernelFPUOpTrait>(),
                "FPUOp must have TTKernelFPUOpTrait");

  static constexpr int arity = FPUOp::arity;

  LogicalResult
  matchAndRewrite(ConcreteOp op, typename ConcreteOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto store = mlir::cast<memref::StoreOp>(*op->user_begin());
    auto outCB = rewriter.getRemappedValue(store.getMemref());

    assert(op->hasOneUse());
    if constexpr (arity == 1) {
      assert(op->getNumOperands() == 1u);
    } else if constexpr (arity == 2) {
      assert(op->getNumOperands() == 2u);
      rewriter.create<ttkernel::BinaryOpInitCommonOp>(
          op->getLoc(), getCB(rewriter, adaptor.getLhs()),
          getCB(rewriter, adaptor.getRhs()), outCB);
    } else {
      static_assert(arity == 3 && !ttmlir::utils::always_false<ConcreteOp>(),
                    "FPUOp must be unary, binary or ternary");
      assert(op->getNumOperands() == 3u);
    }

    auto dstIdx = index(rewriter, op->getLoc(), 0);

    if constexpr (std::is_same_v<ConcreteOp, ttir::TileMatmulOp>) {
      auto insertionPoint = rewriter.getInsertionPoint();
      auto cbA = getCB(rewriter, adaptor.getA());
      auto cbB = getCB(rewriter, adaptor.getB());
      auto cbC = getCB(rewriter, adaptor.getC());
      setInsertionPointAfterOperands(rewriter, {cbA, cbB, cbC});
      rewriter.create<ttkernel::MatmulInitOp>(
          op->getLoc(), cbA, cbB, cbC,
          /* transpose */ i32(rewriter, op->getLoc(), 0));
      rewriter.setInsertionPoint(insertionPoint->getBlock(), insertionPoint);
      auto mmInitShortOp = rewriter.create<ttkernel::MatmulInitShortOp>(
          op->getLoc(), cbA, cbB,
          /* transpose */ i32(rewriter, op->getLoc(), 0));
      rewriter.create<ttkernel::MatmulTilesOp>(
          op->getLoc(), cbA, cbB, getLoadIndex(adaptor.getA()),
          getLoadIndex(adaptor.getB()), dstIdx,
          /* transpose */ i32(rewriter, op->getLoc(), 0));
      rewriter.setInsertionPoint(mmInitShortOp);
      lowerLoadToCopyTile(
          adaptor.getC().template getDefiningOp<memref::LoadOp>(), false,
          rewriter);
    } else if constexpr (arity == 2) {
      rewriter.create<InitOp>(op->getLoc(), getCB(rewriter, adaptor.getLhs()),
                              getCB(rewriter, adaptor.getRhs()));
      rewriter.create<FPUOp>(op->getLoc(), getCB(rewriter, adaptor.getLhs()),
                             getCB(rewriter, adaptor.getRhs()),
                             getLoadIndex(adaptor.getLhs()),
                             getLoadIndex(adaptor.getRhs()), dstIdx);
    } else {
      return llvm::failure();
    }

    rewriter.eraseOp(op);
    return success();
  }
};

} // namespace

namespace {

template <typename ConcreteOp, typename InitOp, typename SFPUOp>
class TTIRSFPUOpsRewriter : public OpConversionPattern<ConcreteOp> {
public:
  using OpConversionPattern<ConcreteOp>::OpConversionPattern;
  static_assert(SFPUOp::template hasTrait<TTKernelSFPUOpTrait>(),
                "SFPUOp must have TTKernelSFPUOpTrait");

  static constexpr int arity = SFPUOp::arity;

  static_assert(arity == 1 || arity == 2,
                "Only unary and binary SFPUOps are supported");

  LogicalResult
  matchAndRewrite(ConcreteOp op, typename ConcreteOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Operation *newOp = nullptr;
    Operation *initOp = nullptr;

    auto load =
        mlir::cast<memref::LoadOp>((*op->operand_begin()).getDefiningOp());
    auto inCB = rewriter.getRemappedValue(load.getMemref());
    auto store = mlir::cast<memref::StoreOp>(*op->user_begin());
    auto outCB = rewriter.getRemappedValue(store.getMemref());
    assert(inCB.getDefiningOp()->isBeforeInBlock(outCB.getDefiningOp()));
    rewriter.setInsertionPointAfter(outCB.getDefiningOp());
    rewriter.create<ttkernel::InitSFPUOp>(op->getLoc(), inCB, outCB);

    if constexpr (arity == 1) {
      initOp = rewriter.create<InitOp>(op->getLoc());
      newOp =
          rewriter.create<SFPUOp>(op->getLoc(), i32(rewriter, op->getLoc(), 0));
    } else {
      initOp = rewriter.create<InitOp>(op->getLoc());
      newOp =
          rewriter.create<SFPUOp>(op->getLoc(), i32(rewriter, op->getLoc(), 0),
                                  i32(rewriter, op->getLoc(), 1));
    }

    rewriter.setInsertionPoint(initOp == nullptr ? newOp : initOp);
    for (int i = 0; i < arity; i++) {
      lowerLoadToCopyTile(
          adaptor.getOperands()[i].template getDefiningOp<memref::LoadOp>(),
          true, rewriter);
    }

    rewriter.eraseOp(op);
    return success();
  }
};

} // namespace

namespace {

class TTIRTilizeUntilizeRewriter
    : public OpTraitConversionPattern<
          mlir::tt::ttir::TTIRGenericRegionComputeOpTrait> {
public:
  using OpTraitConversionPattern<
      mlir::tt::ttir::TTIRGenericRegionComputeOpTrait>::
      OpTraitConversionPattern;

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final {
    if (mlir::isa<ttir::TileTilizeBlockOp>(op)) {
      assert(operands.size() == 2);
      Value src = operands[0];
      Value dst = operands[1];
      auto numTiles =
          i32(rewriter, op->getLoc(),
              mlir::cast<ttkernel::CBType>(dst.getType()).getNumTiles());
      rewriter.create<ttkernel::TilizeInitOp>(op->getLoc(), src, numTiles, dst);
      rewriter.create<ttkernel::TilizeBlockOp>(op->getLoc(), src, numTiles,
                                               dst);
    } else if (mlir::isa<ttir::TileUntilizeBlockOp>(op)) {
      assert(operands.size() == 2);
      Value src = operands[0];
      Value dst = operands[1];
      auto numTiles =
          i32(rewriter, op->getLoc(),
              mlir::cast<ttkernel::CBType>(src.getType()).getNumTiles());
      rewriter.create<ttkernel::UntilizeInitOp>(op->getLoc(), src, dst);
      rewriter.create<ttkernel::UntilizeBlockOp>(op->getLoc(), src, numTiles,
                                                 dst);
    } else {
      return failure();
    }

    // This is necessary to remove the invalid CollapseShapeOp that references a
    // CB once it has no more uses.
    for (uint32_t i = 0; i < op->getNumOperands(); i++) {
      auto load = operands[i].getDefiningOp<memref::LoadOp>();
      if (load) {
        rewriter.eraseOp(load);
      }
    }

    rewriter.eraseOp(op);
    return success();
  };
};
} // namespace

namespace {

template <typename ConcreteOp>
class TTIRAwaitYieldRewriter : public OpConversionPattern<ConcreteOp> {
public:
  using OpConversionPattern<ConcreteOp>::OpConversionPattern;

  static_assert(std::is_same_v<ConcreteOp, ttir::AwaitOp> ||
                    std::is_same_v<ConcreteOp, ttir::YieldOp>,
                "Expected Await or Yield op passed to TTIRAwaitYieldRewriter.");

  LogicalResult
  matchAndRewrite(ConcreteOp op, typename ConcreteOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto device = lookupDevice(op);
    for (Value input : adaptor.getValues()) {
      auto cb = mlir::dyn_cast<ttkernel::CBType>(input.getType());
      assert(cb && "Expected CB input type to await/yield, failing.");
      auto memref = cb.getMemref();
      auto cbNumPages = device.getMemrefCBNumPages(memref);
      auto numPages = i32(rewriter, op->getLoc(), cbNumPages);
      Block *block = op->getBlock();
      if (mlir::isa<ttir::AwaitOp>(op)) {
        rewriter.create<ttkernel::CBWaitFrontOp>(op.getLoc(), input, numPages);
        auto popFront = rewriter.create<ttkernel::CBPopFrontOp>(
            op.getLoc(), input, numPages);
        rewriter.moveOpBefore(popFront, block->getTerminator());
      } else if (mlir::isa<ttir::YieldOp>(op)) {
        auto reserveBack = rewriter.create<ttkernel::CBReserveBackOp>(
            op.getLoc(), input, numPages);
        if (mlir::isa<func::FuncOp>(block->getParentOp())) {
          rewriter.moveOpAfter(reserveBack, input.getDefiningOp());
        } else {
          rewriter.moveOpBefore(reserveBack, &block->front());
        }
        rewriter.moveOpBefore(numPages.getDefiningOp(), reserveBack);
        rewriter.create<ttkernel::CBPushBackOp>(op.getLoc(), input, numPages);
      }
    }

    rewriter.eraseOp(op);
    return success();
  };
};

} // namespace

namespace {
class TTIRDMARewriter : public OpConversionPattern<ttir::DMAOp> {
public:
  TTIRDMARewriter(TypeConverter &typeConverter, MLIRContext *context,
                  const ttir::AssociatedDMAWaits *associatedDMAWaits)
      : OpConversionPattern<ttir::DMAOp>(typeConverter, context),
        associatedDMAWaits(associatedDMAWaits) {}

  static size_t getElementSizeBytes(MemRefType memref) {
    mlir::Type elementType = memref.getElementType();
    auto tileType = mlir::dyn_cast<TileType>(elementType);
    return tileType ? tileType.getSizeBytes()
                    : elementType.getIntOrFloatBitWidth() / 8;
  }

  static int64_t getLocalMemrefSizeBytes(MemRefType memref) {
    return getElementSizeBytes(memref) * memref.getNumElements();
  }

  // For use on REMOTE memrefs
  static size_t getMemrefShardNumElems(MemRefType memref) {
    DeviceLayoutInterface layout =
        mlir::cast<DeviceLayoutInterface>(memref.getLayout());
    return layout.getShardNumElements(memref);
  }

  static std::tuple<AffineMap, AffineMap, AffineMap>
  getIndividualResultMaps(Operation *op, tt::DeviceAttr device,
                          OpBuilder &builder) {
    std::pair<MemRefType, AffineMap> memrefAndView = ttir::applyViews(op);
    size_t pageSize =
        device.getMemrefSizeBytes(memrefAndView.first, /*pageSize=*/0);
    AffineMap memoryMap = device.getMemoryMap(memrefAndView, pageSize, 0)
                              .dropResult(0); // drop the device index
    assert(memoryMap.getNumResults() == 3);
    auto gridY = memoryMap.dropResults({1, 2});
    auto gridX = memoryMap.dropResults({0, 2});
    auto offset = memoryMap.dropResults({0, 1});
    return std::make_tuple(gridY, gridX, offset);
  }

  static Value castCBTypeAsAddress(OpBuilder &rewriter, Location loc,
                                   Value cb) {
    // This is required because we blanket convert Memrefs into CBs with a type
    // converter, however there are actually two paths a memref can take:
    // 1. It can be a CBType, which is the case for local memrefs
    // 2. It can represent remote data, which we need to lower to a compile time
    // address (I32 type)
    // More information on ticket #3172
    return rewriter
        .create<UnrealizedConversionCastOp>(loc, rewriter.getI32Type(), cb)
        ->getResult(0);
  }

  LogicalResult
  matchAndRewrite(ttir::DMAOp op, ttir::DMAOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto device = lookupDevice(op);
    auto systemDesc = getCurrentScopeSystemDesc(op);
    auto chipIds = device.getChipIds();
    auto chipDescs = systemDesc.getChipDescs();
    auto chipDescIndices = systemDesc.getChipDescIndices();
    assert(chipIds.size() == 1);
    auto chipDesc = chipDescs[chipDescIndices[chipIds[0]]];

    // TODO(jdesousa): Temporary L1 assertion until DRAM is supported
    assert(isL1MemorySpace(mlir::cast<MemorySpaceAttr>(
                               op.getSrc().getType().getMemorySpace())
                               .getValue()) &&
           isL1MemorySpace(mlir::cast<MemorySpaceAttr>(
                               op.getDst().getType().getMemorySpace())
                               .getValue()) &&
           "Expected src and dst memory spaces to be L1, failing.");

    auto applyMap = [&](AffineMap map, ValueRange index) {
      auto apply =
          rewriter.create<affine::AffineApplyOp>(op.getLoc(), map, index);
      return apply;
    };

    bool isRead = false;
    if (op.isSrcLocal() && op.isDstLocal()) {
      // Local to Local Datamovement & Multicast

      auto srcCb = mlir::cast<ttkernel::CBType>(adaptor.getSrc().getType());

      // Both src and dst are local, use the metal cb pointers to determine
      // addressing
      Value srcL1Start = rewriter.create<ttkernel::GetReadPtrOp>(
          op.getLoc(), adaptor.getSrc());
      Value dstL1Start = rewriter.create<ttkernel::GetWritePtrOp>(
          op.getLoc(), adaptor.getDst());

      Value transferSize = i32(rewriter, op->getLoc(),
                               getLocalMemrefSizeBytes(srcCb.getMemref()));
      if (op.isMcast()) {
        // Multicast lowering
        // Get virtual start coordinates from DMA op logical coordinates
        auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
            rewriter, op.getLoc(), chipDesc, op.getMcastStartIndex());
        // Get the multicast end coordinates from the virtual start coordinates
        // and mcast shape
        auto [mcastEndY, mcastEndX] = getMcastEndCoords(
            rewriter, op.getLoc(), virtY, virtX, op.getMcastShape());
        auto numDestsIdx = rewriter.create<arith::MulIOp>(
            op.getLoc(), op.getMcastShape()[0], op.getMcastShape()[1]);
        auto numDests = rewriter.create<arith::IndexCastOp>(
            op.getLoc(), rewriter.getI32Type(), numDestsIdx);
        auto mcastAddr = rewriter.create<ttkernel::GetNocMulticastAddrOp>(
            op.getLoc(), virtX, virtY, mcastEndX, mcastEndY, dstL1Start,
            nullptr);
        if (adaptor.getSrc() == adaptor.getDst()) {
          // If src and dst refer to the same memref, we do not loopback mcast
          // Dests are one less because the sender core is not included
          auto numDestsLessOne = rewriter.create<arith::SubIOp>(
              op.getLoc(), numDests, i32(rewriter, op->getLoc(), 1));
          rewriter.create<ttkernel::NocAsyncWriteMulticastOp>(
              op.getLoc(), srcL1Start, mcastAddr, transferSize, numDestsLessOne,
              nullptr, nullptr, nullptr);
        } else {
          // If src != dst, we loopback mcast
          rewriter.create<ttkernel::NocAsyncWriteMulticastLoopbackSrcOp>(
              op.getLoc(), srcL1Start, mcastAddr, transferSize, numDests,
              nullptr, nullptr, nullptr);
        }
      } else {
        // Local L1 to Local L1 local data movement lowering
        // Get local coordinates using myY and myX ops
        auto myY = rewriter.create<ttir::CoreIndexOp>(
            op.getLoc(), rewriter.getIndexType(),
            rewriter.getI64IntegerAttr(0));
        auto myX = rewriter.create<ttir::CoreIndexOp>(
            op.getLoc(), rewriter.getIndexType(),
            rewriter.getI64IntegerAttr(1));
        // Convert local coordinates to virtual coordinates
        auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
            rewriter, op.getLoc(), chipDesc, ValueRange{myY, myX});
        auto nocAddr = rewriter.create<ttkernel::GetNocAddrOp>(
            op.getLoc(), virtX, virtY, dstL1Start);
        rewriter.create<ttkernel::NocAsyncWriteOp>(op.getLoc(), srcL1Start,
                                                   nocAddr, transferSize);
      }
    } else if (op.isSrcLocal() && op.isDstRemote()) {
      // Local to Remote write
      if (!op.getOptNumElems()) {
        op.setOptNumElems(getMemrefShardNumElems(op.getDst().getType()));
      }
      auto transferSize =
          i32(rewriter, op->getLoc(),
              op.getNumElems() * getElementSizeBytes(op.getDstMemRefType()));

      // Use the cb addr as the read address since it is local
      Value srcL1Start = rewriter.create<ttkernel::GetReadPtrOp>(
          op.getLoc(), adaptor.getSrc());

      // Use the affine mapping from the dst memref to get the dst address and
      // coordinates
      AffineMap dstGridYMap, dstGridXMap, dstOffsetMap;
      std::tie(dstGridYMap, dstGridXMap, dstOffsetMap) =
          getIndividualResultMaps(op.getDst().getDefiningOp(), device,
                                  rewriter);

      auto dstGridY = applyMap(dstGridYMap, op.getDstIndices());
      auto dstGridX = applyMap(dstGridXMap, op.getDstIndices());
      auto dstOffset = applyMap(dstOffsetMap, op.getDstIndices());
      auto dstOffsetInt = rewriter.create<arith::IndexCastOp>(
          op.getLoc(), rewriter.getI32Type(), dstOffset);

      auto dstAddrAsInt =
          castCBTypeAsAddress(rewriter, op->getLoc(), adaptor.getDst());
      auto dstAddr = rewriter.create<arith::AddIOp>(op.getLoc(), dstOffsetInt,
                                                    dstAddrAsInt);

      // Translate the dst coordinates to virtual coordinates
      auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
          rewriter, op.getLoc(), chipDesc, ValueRange{dstGridY, dstGridX});
      auto nocAddr = rewriter.create<ttkernel::GetNocAddrOp>(op.getLoc(), virtX,
                                                             virtY, dstAddr);
      rewriter.create<ttkernel::NocAsyncWriteOp>(op.getLoc(), srcL1Start,
                                                 nocAddr, transferSize);
    } else if (op.isSrcRemote() && op.isDstLocal()) {
      // Remote to Local read
      if (!op.getOptNumElems()) {
        op.setOptNumElems(getMemrefShardNumElems(op.getSrc().getType()));
      }

      // Use the cb addr as the write address since it is local
      Value dstL1Start = rewriter.create<ttkernel::GetWritePtrOp>(
          op.getLoc(), adaptor.getDst());

      // Use the affine mapping from the src memref to get the src address and
      // coordinates
      AffineMap srcGridYMap, srcGridXMap, srcOffsetMap;
      std::tie(srcGridYMap, srcGridXMap, srcOffsetMap) =
          getIndividualResultMaps(op.getSrc().getDefiningOp(), device,
                                  rewriter);

      auto srcGridY = applyMap(srcGridYMap, op.getSrcIndices());
      auto srcGridX = applyMap(srcGridXMap, op.getSrcIndices());
      auto srcOffset = applyMap(srcOffsetMap, op.getSrcIndices());
      auto srcOffsetInt = rewriter.create<arith::IndexCastOp>(
          op.getLoc(), rewriter.getI32Type(), srcOffset);
      auto size =
          i32(rewriter, op->getLoc(),
              op.getNumElems() * getElementSizeBytes(op.getSrcMemRefType()));

      auto srcAddrAsInt =
          castCBTypeAsAddress(rewriter, op->getLoc(), adaptor.getSrc());
      auto srcAddr = rewriter.create<arith::AddIOp>(op.getLoc(), srcOffsetInt,
                                                    srcAddrAsInt);

      // Translate the src coordinates to virtual coordinates
      auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
          rewriter, op.getLoc(), chipDesc, ValueRange{srcGridY, srcGridX});
      auto srcNocAddr = rewriter.create<ttkernel::GetNocAddrOp>(
          op.getLoc(), virtX, virtY, srcAddr);
      rewriter.create<ttkernel::NocAsyncReadOp>(op.getLoc(), srcNocAddr,
                                                dstL1Start, size);
      isRead = true;
    } else {
      emitError(op.getLoc(), "Unsupported DMA Configuration");
      return failure();
    }

    // Add attribute marking whether the DMA wait is for a read or write
    // operation This will be used when loweing the wait ops because the current
    // DMA op will be replaced with a NullTx.
    auto dmaWaitOps = associatedDMAWaits->get(op);
    for (auto dmaWaitOp : dmaWaitOps) {
      rewriter.modifyOpInPlace(dmaWaitOp, [&]() {
        if (isRead) {
          dmaWaitOp->setDiscardableAttr("ttkernel.lowering.associated_noc_read",
                                        rewriter.getUnitAttr());
        } else {
          dmaWaitOp->setDiscardableAttr(
              "ttkernel.lowering.associated_noc_write", rewriter.getUnitAttr());
        }
      });
    }

    rewriter.replaceOpWithNewOp<ttir::NullTxOp>(op);
    return success();
  }

private:
  const ttir::AssociatedDMAWaits *associatedDMAWaits;
};
} // namespace

namespace {
class TTIRCoreIndexRewriter : public OpConversionPattern<ttir::CoreIndexOp> {
public:
  using OpConversionPattern<ttir::CoreIndexOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ttir::CoreIndexOp op, ttir::CoreIndexOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto device = lookupDevice(op);
    auto systemDesc = getCurrentScopeSystemDesc(op);
    auto chipIds = device.getChipIds();
    auto chipDescs = systemDesc.getChipDescs();
    auto chipDescIndices = systemDesc.getChipDescIndices();
    assert(chipIds.size() == 1);
    auto chipDesc = chipDescs[chipDescIndices[chipIds[0]]];

    assert(op.getDim() == 0 ||
           op.getDim() == 1 &&
               "Expected core index dim to be in range 0-1, failing.");
    if (op.getDim()) {
      auto coreIndex = rewriter.create<ttkernel::MyXOp>(op.getLoc(), nullptr);
      auto normalizedCoreIndex = rewriter.create<arith::SubIOp>(
          op.getLoc(), coreIndex,
          index(rewriter, op->getLoc(),
                chipDesc.getCoordTranslationOffsets()[1]));
      rewriter.replaceOp(op, normalizedCoreIndex);
    } else {
      auto coreIndex = rewriter.create<ttkernel::MyYOp>(op.getLoc(), nullptr);
      auto normalizedCoreIndex = rewriter.create<arith::SubIOp>(
          op.getLoc(), coreIndex,
          index(rewriter, op->getLoc(),
                chipDesc.getCoordTranslationOffsets()[0]));
      rewriter.replaceOp(op, normalizedCoreIndex);
    }
    return success();
  }
};
} // namespace

namespace {
class TTIRDMAWaitRewriter : public OpConversionPattern<ttir::DMAWaitOp> {
public:
  using OpConversionPattern<ttir::DMAWaitOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ttir::DMAWaitOp op, ttir::DMAWaitOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto isRead =
        op->getDiscardableAttr("ttkernel.lowering.associated_noc_read");
    auto isWrite =
        op->getDiscardableAttr("ttkernel.lowering.associated_noc_write");
    assert(isRead || isWrite);

    if (isRead) {
      rewriter.create<ttkernel::NocAsyncReadBarrierOp>(op.getLoc());
    }

    if (isWrite) {
      rewriter.create<ttkernel::NocAsyncWriteBarrierOp>(op.getLoc());
    }

    rewriter.eraseOp(op);
    return success();
  }
};
} // namespace

namespace {
class TTIRGetGlobalOperandRewriter
    : public OpConversionPattern<ttir::GetGlobalOperandOp> {
public:
  using OpConversionPattern<ttir::GetGlobalOperandOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ttir::GetGlobalOperandOp op,
                  ttir::GetGlobalOperandOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    func::FuncOp entry = op->getParentOfType<func::FuncOp>();
    auto arg =
        rewriter.getAttr<ArgAttr>(ArgType::BufferAddress, op.getOperandIndex());
    size_t argIndex;
    rewriter.modifyOpInPlace(entry, [&]() {
      argIndex = ArgSpecAttr::appendCompileTimeArg(entry, arg);
    });

    rewriter.replaceOpWithNewOp<ttkernel::GetCompileArgValOp>(
        op, rewriter.getI32Type(), argIndex);

    return success();
  }
};
} // namespace

namespace {
class TTIRNullTxRewriter : public OpConversionPattern<ttir::NullTxOp> {
public:
  using OpConversionPattern<ttir::NullTxOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ttir::NullTxOp op, ttir::NullTxOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, rewriter.getIndexType(),
                                                   rewriter.getIndexAttr(0));
    return success();
  }
};
} // namespace

namespace {
class MemRefCollapseRewriter
    : public OpConversionPattern<memref::CollapseShapeOp> {
public:
  using OpConversionPattern<memref::CollapseShapeOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(memref::CollapseShapeOp op,
                  memref::CollapseShapeOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    rewriter.replaceOpWithNewOp<CBReinterpretShapeOp>(
        op, getTypeConverter()->convertType(op.getResult().getType()),
        adaptor.getSrc());
    return success();
  }
};
} // namespace

namespace {
class TTIRKernelFunctionArgsRewriter
    : public OpConversionPattern<func::FuncOp> {
public:
  using OpConversionPattern<func::FuncOp>::OpConversionPattern;

  static void convertFunctionAttrs(Builder &builder, func::FuncOp op,
                                   ArrayRef<ArgAttr> rtArgs,
                                   ArrayRef<ArgAttr> ctArgs) {
    ttir::ThreadAttr threadAttr =
        op->getAttrOfType<ttir::ThreadAttr>(ttir::ThreadAttr::name);
    op->removeAttr(ttir::ThreadAttr::name);
    ThreadType threadType;
    switch (threadAttr.getThreadType()) {
    case ttir::ThreadType::Compute: {
      threadType = ThreadType::Compute;
      break;
    }
    case ttir::ThreadType::Datamovement: {
      threadType = ThreadType::Noc;
      break;
    }
    }
    op->setAttr(ThreadTypeAttr::name,
                builder.getAttr<ThreadTypeAttr>(threadType));
    ArgSpecAttr::setArgSpec(op, builder.getAttr<ArgSpecAttr>(rtArgs, ctArgs));
  }

  LogicalResult
  matchAndRewrite(func::FuncOp op, func::FuncOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    if (!op->hasAttr(ttir::ThreadAttr::name) ||
        (op.getFunctionType().getNumInputs() == 0)) {
      return failure();
    }

    Block *block = &op.getCallableRegion()->front();
    auto blockArgs = block->getArguments();
    assert(!blockArgs.empty());

    SmallVector<ArgAttr> rtArgSpecVector;
    SmallVector<ArgAttr> ctArgSpecVector;
    size_t currentSemaphoreIndex = 0;
    TypeConverter::SignatureConversion signatureConverter(op.getNumArguments());
    OpBuilder::InsertionGuard funcInsertionGuard(rewriter);
    rewriter.setInsertionPointToStart(block);
    for (auto arg : blockArgs) {
      Type argType = getTypeConverter()->convertType(arg.getType());
      if (mlir::isa<CBType>(argType)) {
        auto cb = rewriter.create<GetCompileArgValOp>(
            op.getLoc(), getTypeConverter()->convertType(arg.getType()),
            rewriter.getI32IntegerAttr(arg.getArgNumber()));
        signatureConverter.remapInput(arg.getArgNumber(), cb);
        ctArgSpecVector.push_back(
            rewriter.getAttr<ArgAttr>(ArgType::CBPort, arg.getArgNumber()));
      } else if (mlir::isa<SemaphoreType>(argType)) {
        size_t ctArgIndex = ctArgSpecVector.size();
        auto semaphoreIndex = rewriter.create<GetCompileArgValOp>(
            op.getLoc(), rewriter.getI32Type(),
            rewriter.getI32IntegerAttr(ctArgIndex));
        auto semaphore =
            rewriter.create<GetSemaphoreOp>(op.getLoc(), semaphoreIndex);
        signatureConverter.remapInput(arg.getArgNumber(),
                                      semaphore.getResult());
        ctArgSpecVector.push_back(rewriter.getAttr<ArgAttr>(
            ArgType::Semaphore, currentSemaphoreIndex++));
      } else {
        llvm_unreachable("unexpected block argument type");
      }
    }

    rewriter.applySignatureConversion(block, signatureConverter,
                                      getTypeConverter());
    rewriter.modifyOpInPlace(op, [&]() {
      op.setType(rewriter.getFunctionType(TypeRange(), TypeRange()));
      convertFunctionAttrs(rewriter, op, rtArgSpecVector, ctArgSpecVector);
    });
    return success();
  }
};
} // namespace

namespace {
template <typename ConcreteOp>
class TTIRSemaphoreUpdateRewriter : public OpConversionPattern<ConcreteOp> {
public:
  using OpConversionPattern<ConcreteOp>::OpConversionPattern;

  static_assert(std::is_same_v<ConcreteOp, ttir::SemaphoreSetOp> ||
                    std::is_same_v<ConcreteOp, ttir::SemaphoreIncOp>,
                "Expected SemaphoreSet or SemaphoreInc op passed to "
                "TTIRSemaphoreUpdateRewriter.");

  LogicalResult
  matchAndRewrite(ConcreteOp op, typename ConcreteOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    auto device = lookupDevice(op);
    auto systemDesc = getCurrentScopeSystemDesc(op);
    auto chipIds = device.getChipIds();
    auto chipDescs = systemDesc.getChipDescs();
    auto chipDescIndices = systemDesc.getChipDescIndices();
    assert(chipIds.size() == 1);
    auto chipDesc = chipDescs[chipDescIndices[chipIds[0]]];

    Value value = op.getValue();
    Value semaphoreAddr = adaptor.getSemaphore();

    if (op.getDstCoreIndex().empty()) {
      assert(!mlir::isa<ttir::SemaphoreIncOp>(op) &&
             "ttir.semaphore_inc to local core is illegal.");

      // Local semaphore set
      auto semaphorePtr =
          rewriter.create<ttkernel::CastToL1PtrOp>(op.getLoc(), semaphoreAddr);

      rewriter.replaceOpWithNewOp<ttkernel::NocSemaphoreSetOp>(op, semaphorePtr,
                                                               value);
    } else if (op.getMcastShape().empty()) {
      assert(!mlir::isa<ttir::SemaphoreSetOp>(op) &&
             "ttir.semaphore_set to single remote core is illegal.");
      auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
          rewriter, op.getLoc(), chipDesc, op.getDstCoreIndex());
      auto nocAddr = rewriter.create<ttkernel::GetNocAddrOp>(
          op.getLoc(), virtX, virtY, semaphoreAddr);
      rewriter.replaceOpWithNewOp<ttkernel::NocSemaphoreIncOp>(op, nocAddr,
                                                               value, nullptr);
    } else {
      assert(!mlir::isa<ttir::SemaphoreIncOp>(op) &&
             "ttir.semaphore_inc multicast is illegal.");

      auto [virtY, virtX] = getVirtualCoordsFromLogicalCoords(
          rewriter, op.getLoc(), chipDesc, op.getDstCoreIndex());
      auto [mcastEndY, mcastEndX] = getMcastEndCoords(
          rewriter, op.getLoc(), virtY, virtX, op.getMcastShape());
      Value numDestsIdx = rewriter.create<arith::MulIOp>(
          op.getLoc(), op.getMcastShape()[0], op.getMcastShape()[1]);
      Value numDests = rewriter.create<arith::IndexCastOp>(
          op.getLoc(), rewriter.getI32Type(), numDestsIdx);
      auto mcastAddr = rewriter.create<ttkernel::GetNocMulticastAddrOp>(
          op.getLoc(), virtX, virtY, mcastEndX, mcastEndY, semaphoreAddr,
          nullptr);

      auto semaphorePtr =
          rewriter.create<ttkernel::CastToL1PtrOp>(op.getLoc(), semaphoreAddr);
      rewriter.create<ttkernel::NocSemaphoreSetOp>(op.getLoc(), semaphorePtr,
                                                   value);
      rewriter.replaceOpWithNewOp<ttkernel::NocSemaphoreSetMulticastOp>(
          op, semaphoreAddr, mcastAddr, numDests, nullptr, nullptr);
    }

    return success();
  }
};
} // namespace

namespace {
class TTIRSemaphoreWaitRewriter
    : public OpConversionPattern<ttir::SemaphoreWaitOp> {
public:
  using OpConversionPattern<ttir::SemaphoreWaitOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ttir::SemaphoreWaitOp op,
                  ttir::SemaphoreWaitOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {

    Value semaphoreAddr = adaptor.getSemaphore();
    auto semaphorePtr =
        rewriter.create<ttkernel::CastToL1PtrOp>(op.getLoc(), semaphoreAddr);

    rewriter.replaceOpWithNewOp<ttkernel::NocSemaphoreWaitOp>(op, semaphorePtr,
                                                              op.getValue());
    if (op.getResetValue()) {
      rewriter.create<ttkernel::NocSemaphoreSetOp>(op.getLoc(), semaphorePtr,
                                                   op.getResetValue());
    }

    return success();
  }
};
} // namespace

} // namespace mlir::tt::ttkernel

namespace mlir::tt {

void populateTTIRToTTKernelPatterns(
    MLIRContext *ctx, RewritePatternSet &patterns, TypeConverter &typeConverter,
    const ttir::AssociatedDMAWaits &associatedDMAWaits) {
  // clang-format off
  patterns.add<
      ttkernel::TTIRKernelFunctionArgsRewriter,

      // Elementwise FPU.
      ttkernel::TTIRFPUOpsRewriter<ttir::TileAddOp,      ttkernel::AddTilesInitOp, ttkernel::AddTilesOp>,
      ttkernel::TTIRFPUOpsRewriter<ttir::TileMulOp,      ttkernel::MulTilesInitOp, ttkernel::MulTilesOp>,
      ttkernel::TTIRFPUOpsRewriter<ttir::TileMatmulOp,   ttkernel::MatmulInitOp,   ttkernel::MatmulTilesOp>,

      // Elementwise SFPU.
      ttkernel::TTIRSFPUOpsRewriter<ttir::TileDivOp,     ttkernel::DivBinaryTilesInitOp, ttkernel::DivBinaryTilesOp>,
      ttkernel::TTIRSFPUOpsRewriter<ttir::TileMaximumOp, ttkernel::MaxTilesInitOp,       ttkernel::MaxTilesOp>,
      ttkernel::TTIRSFPUOpsRewriter<ttir::TileExpOp,     ttkernel::ExpTileInitOp,        ttkernel::ExpTileOp>,
      ttkernel::TTIRSFPUOpsRewriter<ttir::TileSinOp,     ttkernel::SinTileInitOp,        ttkernel::SinTileOp>,

      ttkernel::TTIRTilizeUntilizeRewriter,
      ttkernel::MemrefStoreRewriter,
      ttkernel::TTIRAwaitYieldRewriter<ttir::AwaitOp>,
      ttkernel::TTIRAwaitYieldRewriter<ttir::YieldOp>,
      ttkernel::TTIRDMAWaitRewriter,
      ttkernel::TTIRCoreIndexRewriter,
      ttkernel::TTIRGetGlobalOperandRewriter,
      ttkernel::TTIRNullTxRewriter,
      ttkernel::MemRefCollapseRewriter,
      ttkernel::TTIRSemaphoreUpdateRewriter<ttir::SemaphoreSetOp>,
      ttkernel::TTIRSemaphoreUpdateRewriter<ttir::SemaphoreIncOp>,
      ttkernel::TTIRSemaphoreWaitRewriter>(typeConverter, ctx);

  patterns.add<ttkernel::TTIRDMARewriter>(typeConverter, ctx, &associatedDMAWaits);
  // clang-format on
}

} // namespace mlir::tt
