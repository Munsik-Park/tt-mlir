// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ttmlir/Dialect/TT/IR/TTOpsTypes.h"
#include "ttmlir/Dialect/TT/IR/Utils.h"
#include "ttmlir/Dialect/TTNN/Analysis/AllPossibleLayoutsAnalysis.h"
#include "ttmlir/Dialect/TTNN/Analysis/Edge.h"
#include "ttmlir/Dialect/TTNN/Analysis/LegalLayoutAnalysis.h"
#include "ttmlir/Dialect/TTNN/Analysis/MemReconfig.h"
#include "ttmlir/Dialect/TTNN/Analysis/MemoryLayoutAnalysis.h"
#include "ttmlir/Dialect/TTNN/Analysis/OpConfig.h"
#include "ttmlir/Dialect/TTNN/Analysis/OpConfigAnalysis.h"
#include "ttmlir/Dialect/TTNN/Analysis/ScalarDataTypeAnalysis.h"
#include "ttmlir/Dialect/TTNN/Analysis/ShardSolver.h"
#include "ttmlir/Dialect/TTNN/Analysis/TensorLayouts.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOps.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOpsAttrs.h"
#include "ttmlir/Dialect/TTNN/IR/TTNNOpsTypes.h"
#include "ttmlir/Dialect/TTNN/Transforms/Passes.h"
#include "ttmlir/Dialect/TTNN/Utils/PassOverrides.h"
#include "ttmlir/Dialect/TTNN/Utils/Utils.h"
#include "ttmlir/Support/Logger.h"
#include "ttmlir/Utils.h"

#include "mlir/Analysis/Liveness.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Visitors.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

namespace mlir::tt::ttnn {

namespace impl {

std::unique_ptr<::mlir::Pass> createTTNNOptimizer();
std::unique_ptr<::mlir::Pass> createTTNNOptimizer(TTNNOptimizerOptions options);

// Go through the ops, set sharding specs for each op based on sharding
// analysis, by updating layout attribute of each op.
//
template <typename DerivedT>
class TTNNOptimizerBase : public ::mlir::OperationPass<::mlir::ModuleOp> {
public:
  using Base = TTNNOptimizerBase;

  TTNNOptimizerBase()
      : ::mlir::OperationPass<::mlir::ModuleOp>(
            ::mlir::TypeID::get<DerivedT>()) {}
  TTNNOptimizerBase(const TTNNOptimizerBase &other)
      : ::mlir::OperationPass<::mlir::ModuleOp>(other) {}
  TTNNOptimizerBase &operator=(const TTNNOptimizerBase &) = delete;
  TTNNOptimizerBase(TTNNOptimizerBase &&) = delete;
  TTNNOptimizerBase &operator=(TTNNOptimizerBase &&) = delete;
  ~TTNNOptimizerBase() override = default;

  /// Returns the command-line argument attached to this pass.
  static constexpr ::llvm::StringLiteral getArgumentName() {
    return ::llvm::StringLiteral("ttnn-optimizer");
  }
  ::llvm::StringRef getArgument() const override { return "ttnn-optimizer"; }

  ::llvm::StringRef getDescription() const override {
    return "Determine op configurations for maximum performance.";
  }

  /// Returns the derived pass name.
  static constexpr ::llvm::StringLiteral getPassName() {
    return ::llvm::StringLiteral("TTNNOptimizer");
  }
  ::llvm::StringRef getName() const override { return "TTNNOptimizer"; }

  /// Support isa/dyn_cast functionality for the derived pass class.
  static bool classof(const ::mlir::Pass *pass) {
    return pass->getTypeID() == ::mlir::TypeID::get<DerivedT>();
  }

  /// A clone method to create a copy of this pass.
  std::unique_ptr<::mlir::Pass> clonePass() const override {
    return std::make_unique<DerivedT>(*static_cast<const DerivedT *>(this));
  }

  /// Return the dialect that must be loaded in the context before this pass.
  void getDependentDialects(::mlir::DialectRegistry &registry) const override {}

  /// Explicitly declare the TypeID for this class. We declare an explicit
  /// private instantiation because Pass classes should only be visible by the
  /// current library.
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TTNNOptimizerBase<DerivedT>)

  TTNNOptimizerBase(TTNNOptimizerOptions options) : TTNNOptimizerBase() {
    overrideInputLayout = std::move(options.overrideInputLayout);
    overrideOutputLayout = std::move(options.overrideOutputLayout);
    overrideConv2dConfig = std::move(options.overrideConv2dConfig);
    memoryLayoutAnalysisEnabled =
        std::move(options.memoryLayoutAnalysisEnabled);
    memReconfigEnabled = std::move(options.memReconfigEnabled);
    memoryLayoutAnalysisPolicy = std::move(options.memoryLayoutAnalysisPolicy);
    maxLegalLayouts = std::move(options.maxLegalLayouts);
    rowMajorEnabled = std::move(options.rowMajorEnabled);
  }

protected:
  ::mlir::Pass::Option<llvm::StringMap<InputLayoutOverrideParams>,
                       mlir::tt::ttnn::InputLayoutOverrideParser>
      overrideInputLayout{
          *this, OptionNames::overrideInputLayout,
          ::llvm::cl::desc(
              "Manually insert memory reconfig op for specific op's operand."),
          ::llvm::cl::init(llvm::StringMap<InputLayoutOverrideParams>())};
  ::mlir::Pass::Option<llvm::StringMap<OutputLayoutOverrideParams>,
                       mlir::tt::ttnn::OutputLayoutOverrideParser>
      overrideOutputLayout{
          *this, OptionNames::overrideOutputLayout,
          ::llvm::cl::desc("Override output tensor layout for specific ops."),
          ::llvm::cl::init(llvm::StringMap<OutputLayoutOverrideParams>())};
  ::mlir::Pass::Option<llvm::StringMap<Conv2dConfigOverrideParams>,
                       mlir::tt::ttnn::Conv2dConfigOverrideParser>
      overrideConv2dConfig{
          *this, OptionNames::overrideConv2dConfig,
          ::llvm::cl::desc("Override Conv2d configuration for specific ops."),
          ::llvm::cl::init(llvm::StringMap<Conv2dConfigOverrideParams>())};
  ::mlir::Pass::Option<bool> memoryLayoutAnalysisEnabled{
      *this, OptionNames::memoryLayoutAnalysisEnabled,
      ::llvm::cl::desc("Enable memory layout optimization."),
      ::llvm::cl::init(false)};
  ::mlir::Pass::Option<bool> memReconfigEnabled{
      *this, OptionNames::memReconfigEnabled,
      ::llvm::cl::desc("Memory layout reconfiguration pass."),
      ::llvm::cl::init(true)};
  ::mlir::Pass::Option<mlir::tt::MemoryLayoutAnalysisPolicyType,
                       mlir::tt::MemoryLayoutAnalysisPolicyTypeParser>
      memoryLayoutAnalysisPolicy{
          *this, OptionNames::memoryLayoutAnalysisPolicy,
          llvm::cl::desc("Specify policy for memory layout analysis."),
          llvm::cl::init(MemoryLayoutAnalysisPolicyType::DFSharding)};
  ::mlir::Pass::Option<int64_t> maxLegalLayouts{
      *this, OptionNames::maxLegalLayouts,
      ::llvm::cl::desc("Override maximum number of sharded layouts for legal "
                       "layout analysis."),
      ::llvm::cl::init(64)};
  ::mlir::Pass::Option<bool> rowMajorEnabled{
      *this, "row-major-enabled",
      ::llvm::cl::desc(
          "Enable row major layout generation in legal layout analysis."),
      ::llvm::cl::init(false)};

private:
  friend std::unique_ptr<::mlir::Pass> createTTNNOptimizer() {
    return std::make_unique<DerivedT>();
  }

  friend std::unique_ptr<::mlir::Pass>
  createTTNNOptimizer(TTNNOptimizerOptions options) {
    return std::make_unique<DerivedT>(std::move(options));
  }
};
} // namespace impl

std::unique_ptr<::mlir::Pass> createTTNNOptimizer() {
  return impl::createTTNNOptimizer();
}

std::unique_ptr<::mlir::Pass>
createTTNNOptimizer(TTNNOptimizerOptions options) {
  return impl::createTTNNOptimizer(std::move(options));
}

class TTNNOptimizer : public impl::TTNNOptimizerBase<TTNNOptimizer> {
public:
  using impl::TTNNOptimizerBase<TTNNOptimizer>::TTNNOptimizerBase;
  void runOnOperation() final {
    // Generate legal OP configuration candidates.
    // Perform memory layout analysis.
    // Perform final configuration analysis.
    // Apply graph transformations based on analysis results.
    //
    assertOverridesValid();

    ModuleOp moduleOp = getOperation();

    // Get the max grid size from the system description.
    //
    GridAttr maxGrid = lookupDevice(moduleOp).getWorkerGrid();

    SystemDescAttr systemDesc = mlir::cast<tt::SystemDescAttr>(
        moduleOp->getAttr(tt::SystemDescAttr::name));
    ChipDescAttr chipDesc = systemDesc.getChipDescs()[0];
    llvm::DenseMap<Operation *, std::vector<OpConfig>> legalConfigs;

    // Step 1: Run ScalarDataTypeAnalysis to collect all scalar types used in
    // the graph
    ScalarDataTypeAnalysis scalarDataTypeAnalysis =
        getAnalysis<ScalarDataTypeAnalysis>();
    scalarDataTypeAnalysis.init(
        ScalarDataTypeAnalysisInput(&overrideOutputLayout));
    auto scalarTypes = scalarDataTypeAnalysis.getResult();

    TTMLIR_TRACE(ttmlir::LogComponent::Optimizer,
                 "ScalarDataTypeAnalysis found {0} unique scalar types",
                 scalarTypes.size());

    // Step 2: Run AllPossibleLayoutsAnalysis to generate layouts for all tensor
    // types
    mlir::tt::ttnn::AllPossibleLayoutsAnalysis allPossibleLayoutsAnalysis =
        getAnalysis<mlir::tt::ttnn::AllPossibleLayoutsAnalysis>();
    allPossibleLayoutsAnalysis.init(
        mlir::tt::ttnn::AllPossibleLayoutsAnalysisInput(maxGrid, &scalarTypes,
                                                        rowMajorEnabled));
    TensorTypeLayoutsMap tensorTypePossibleLayouts =
        allPossibleLayoutsAnalysis.getResult();

    tracePossibleLayouts(tensorTypePossibleLayouts);

    moduleOp->walk([&](func::FuncOp func) {
      // Filter out all const-eval functions.
      if (ttmlir::utils::isConstEvalFunc(func)) {
        return;
      }

      func->walk([&](Operation *op) {
        if (op->getNumResults() == 0) {
          return;
        }

        if (!isa<RankedTensorType>(op->getResult(0).getType())) {
          return;
        }

        if (llvm::isa<ttnn::EmptyOp>(op)) {
          return;
        }

        RankedTensorType tensorType =
            mlir::cast<RankedTensorType>(op->getResult(0).getType());

        // Get all possible layouts for this tensor type
        // Use layouts from the global analysis instead of regenerating per-op
        auto tensorLayouts = tensorTypePossibleLayouts.find(tensorType);
        bool hasLayoutsForTensorType =
            (tensorLayouts != tensorTypePossibleLayouts.end());

        assert(hasLayoutsForTensorType && "No layouts found for tensor type");

        // Run legal layout analysis to select the best layouts
        LegalLayoutAnalysis legalLayoutAnalysis =
            getChildAnalysis<LegalLayoutAnalysis>(op);
        legalLayoutAnalysis.init(LegalLayoutAnalysisInput(
            &tensorLayouts->getSecond(), maxLegalLayouts, &overrideOutputLayout,
            &overrideConv2dConfig, rowMajorEnabled));
        legalConfigs[op] = legalLayoutAnalysis.getResult();
      });
    });

    llvm::DenseMap<func::FuncOp, llvm::SmallVector<Operation *>> opSchedule;
    llvm::DenseMap<Edge, MemReconfigEntry> memReconfigEntryMap;
    std::vector<Operation *> spillToDramOps;

    // Extract override resharding edges
    //
    llvm::DenseSet<Edge> overrideReshardEdges;
    extractReshardEdges(moduleOp, overrideReshardEdges);

    if (memoryLayoutAnalysisEnabled) {
      // Perform memory layout analysis.
      //
      MemoryLayoutAnalysis memoryLayoutAnalysis =
          getAnalysis<MemoryLayoutAnalysis>();
      memoryLayoutAnalysis.init(MemoryLayoutAnalysisInput(
          &tensorTypePossibleLayouts, legalConfigs, chipDesc.getUsableL1Size(),
          overrideReshardEdges, memoryLayoutAnalysisPolicy));
      legalConfigs = memoryLayoutAnalysis.getResult().legalConfigs;
      opSchedule = memoryLayoutAnalysis.getResult().schedule;
      memReconfigEntryMap =
          memoryLayoutAnalysis.getResult().memReconfigEntryMap;
      spillToDramOps = memoryLayoutAnalysis.getResult().spillToDramOps;
    }

    // Manually overriden resharding edges should be added to the
    // memReconfigEntryMap.
    //
    for (const auto &edge : overrideReshardEdges) {
      if (memReconfigEntryMap.count(edge) == 0) {
        auto newEntry = MemReconfigEntry();
        newEntry.setOverridenReconfig(true);
        memReconfigEntryMap.try_emplace(edge, newEntry);
      }
    }

    // Pick optimal op configuration.
    //
    OpConfigAnalysis opConfigAnalysis = getAnalysis<OpConfigAnalysis>();
    opConfigAnalysis.init(OpConfigAnalysisInput(std::move(legalConfigs)));

    // Pure application of determined grid sizes to the operations.
    // No further analysis.
    //
    moduleOp->walk([&](func::FuncOp func) {
      if (ttmlir::utils::isConstEvalFunc(func)) {
        return;
      }

      SmallVector<Type> funcResultTypes;

      // If schedule is set, apply order of operations to func.
      //
      if (opSchedule[func].size() > 1) {

        for (size_t i = 0; i < opSchedule[func].size() - 1; i++) {
          Operation *op = opSchedule[func][i];

          Operation *nextOp = opSchedule[func][i + 1];
          nextOp->moveAfter(op);

          // Move DPS operand with the op.
          //
          if (isa<mlir::DestinationStyleOpInterface>(nextOp)) {
            nextOp->getOperands().back().getDefiningOp()->moveBefore(nextOp);
          }
        }
      }

      func->walk([&](Operation *op) {
        if (op->getNumResults() == 0) {
          func::ReturnOp funcReturn = dyn_cast<func::ReturnOp>(op);
          if (funcReturn) {
            funcResultTypes.append(funcReturn.getOperandTypes().begin(),
                                   funcReturn.getOperandTypes().end());
          }
          return;
        }

        // Skip empty ops. Handled via DPS op output operand update.
        //
        if (isa<EmptyOp>(op)) {
          return;
        }

        if (!isa<RankedTensorType>(op->getResult(0).getType())) {
          return;
        }

        RankedTensorType tensorType =
            mlir::cast<RankedTensorType>(op->getResult(0).getType());
        llvm::ArrayRef<int64_t> tensorShape = tensorType.getShape();

        // Update the output layout attribute with the new one.
        //
        if (opConfigAnalysis.getResult().contains(op)) {
          RankedTensorType newTensorType = RankedTensorType::get(
              tensorShape,
              opConfigAnalysis.getResult()
                  .at(op)
                  .outputLayout.getScalarElementType(),
              opConfigAnalysis.getResult().at(op).outputLayout);

          // Update the memory space and layout of the op.
          //
          TTNNLayoutAttr layoutAttr =
              mlir::cast<TTNNLayoutAttr>(newTensorType.getEncoding());

          op->getResult(0).setType(newTensorType);

          // Update DPS operand layout as well.
          //
          if (isa<mlir::DestinationStyleOpInterface>(op)) {
            BufferType bufferType = layoutAttr.getBufferType();
            TensorMemoryLayoutAttr tensorMemoryLayoutAttr =
                layoutAttr.getMemLayout();

            op->getOperands().back().setType(newTensorType);
            EmptyOp emptyOp =
                mlir::cast<EmptyOp>(op->getOperands().back().getDefiningOp());

            emptyOp.setDtype(layoutAttr.getDataType());
            if (layoutAttr.isTiled()) {
              emptyOp.setLayout(ttnn::Layout::Tile);
            } else {
              emptyOp.setLayout(ttnn::Layout::RowMajor);
            }
            emptyOp.setMemoryConfigAttr(ttnn::MemoryConfigAttr::get(
                op->getContext(),
                BufferTypeAttr::get(op->getContext(), bufferType),
                ShardSpecAttr::get(
                    op->getContext(),
                    ShapeAttr::get(op->getContext(),
                                   layoutAttr.getMemref().getShape())),
                tensorMemoryLayoutAttr));
          }
          // TODO(mtopalovic): Temp workaround for generic ToLayoutOp. Allign
          // MemoryConfigAttr with layout attribute of its output tensor. This
          // redundant info should be removed or made consistent as part of temp
          // ToLayoutOp decomposition pass.
          //
          else if (isa<ttnn::ToLayoutOp>(op)) {
            BufferType bufferType = layoutAttr.getBufferType();
            TensorMemoryLayoutAttr tensorMemoryLayoutAttr =
                layoutAttr.getMemLayout();
            // Update the device op with the new tensor type.
            //
            ttnn::ToLayoutOp toLayoutOp = llvm::cast<ttnn::ToLayoutOp>(op);
            toLayoutOp.setMemoryConfigAttr(ttnn::MemoryConfigAttr::get(
                op->getContext(),
                ttnn::BufferTypeAttr::get(op->getContext(), bufferType),
                ttnn::ShardSpecAttr::get(
                    op->getContext(),
                    ttnn::ShapeAttr::get(op->getContext(),
                                         layoutAttr.getMemref().getShape())),
                tensorMemoryLayoutAttr));
          }

          // Set specific Conv2d Op configuration if it is exists.
          //
          if (auto conv2dOp = mlir::dyn_cast<ttnn::Conv2dOp>(op)) {
            if (auto conv2dConfig =
                    mlir::dyn_cast_if_present<ttnn::Conv2dConfigAttr>(
                        opConfigAnalysis.getResult().at(op).opSpecificAttr)) {
              conv2dOp.setConv2dConfigAttr(conv2dConfig);
            }
          }
        }
      });

      if (memReconfigEnabled) {
        processMemReconfigEdges(memReconfigEntryMap);
      }

      processSpillOps(spillToDramOps);

      // Update the function type to reflect the updated return operation's
      // result types.
      //
      FunctionType funcType = func.getFunctionType();
      FunctionType newFuncType = FunctionType::get(
          func.getContext(), funcType.getInputs(), funcResultTypes);
      func.setType(newFuncType);
    });
  }

private:
  void assertOverridesValid() {
    // Check if each overriden op exists in the graph.
    // Check if each conv2d config override is applied only to conv2d op.
    //
    llvm::StringMap<bool> overridenOpExists;
    llvm::StringMap<bool> overrideConv2dOp;
    for (const auto &[opLoc, _] : overrideOutputLayout) {
      overridenOpExists[opLoc] = false;
    }
    for (const auto &[opLoc, _] : overrideInputLayout) {
      overridenOpExists[opLoc] = false;
    }
    for (const auto &[opLoc, _] : overrideConv2dConfig) {
      overridenOpExists[opLoc] = false;
      overrideConv2dOp[opLoc] = false;
    }

    ModuleOp moduleOp = getOperation();
    moduleOp->walk([&](Operation *op) {
      if (not isa<NameLoc>(op->getLoc())) {
        return;
      }

      StringRef opLocName = mlir::cast<NameLoc>(op->getLoc()).getName();
      if (overridenOpExists.contains(opLocName)) {
        overridenOpExists[opLocName] = true;
      }
      if (!isa<ttnn::Conv2dOp>(op) && overrideConv2dOp.contains(opLocName)) {
        op->emitRemark() << "Trying to override non-conv2d op: '"
                         << op->getName()
                         << "' with conv2d config. Skipping...";
        return;
      }
    });

    for (const auto &[opLoc, opOverridenAndExists] : overridenOpExists) {
      if (!opOverridenAndExists) {
        llvm::report_fatal_error("Trying to override non-existing op: " +
                                 opLoc + ". Check logs for details");
      }
    }
  }

  void extractReshardEdges(ModuleOp &moduleOp,
                           llvm::DenseSet<Edge> &overrideReshardEdges) {
    moduleOp->walk([&](Operation *op) {
      if (isa<ToLayoutOp>(op)) {
        return;
      }

      // Skip ops without location
      //
      if (!isa<NameLoc>(op->getLoc())) {
        return;
      }

      StringRef opLocName = mlir::cast<NameLoc>(op->getLoc()).getName();
      auto opInputOverride = overrideInputLayout.find(opLocName);

      if (opInputOverride == overrideInputLayout.end()) {
        return;
      }

      SmallVector<int64_t> operandIndexes =
          opInputOverride->getValue().operandIdxes;
      for (int64_t operandIndex : operandIndexes) {
        Value operand = op->getOperand(operandIndex);
        Operation *operandOp = operand.getDefiningOp();
        overrideReshardEdges.insert(Edge(operandOp, op, operandIndex));
      }
    });

    // Check for non-existing ops in override
    //
    assert(overrideInputLayout.size() == overrideReshardEdges.size());
  }

  mlir::TypedValue<DeviceType> getOrCreateDeviceOpValue(Operation *contextOp,
                                                        OpBuilder &builder) {
    Block *block = contextOp->getBlock();
    for (auto &op : block->getOperations()) {
      if (GetDeviceOp deviceOp = dyn_cast<GetDeviceOp>(op)) {
        return deviceOp.getResult();
      }
    }

    // Device op does not exist in the block, hence we need to create it.
    DeviceAttr deviceAttr = lookupDevice(contextOp);
    auto currentInsertionPoint = builder.saveInsertionPoint();
    builder.setInsertionPoint(block, block->begin());
    llvm::SmallVector<int64_t> meshShape{deviceAttr.getMeshShape()};
    if (meshShape.empty()) {
      meshShape = llvm::SmallVector<int64_t, 2>{1, 1};
    }
    // TODO (jnie): Currently hardcoding the mesh offset to 0x0
    // Need a proper plan to dynamically determine this.
    llvm::SmallVector<int64_t, 2> meshOffset{0, 0};

    auto deviceOp = builder.create<ttnn::GetDeviceOp>(
        contextOp->getLoc(), builder.getType<DeviceType>(),
        ttnn::MeshShapeAttr::get(contextOp->getContext(), meshShape[0],
                                 meshShape[1]),
        ttnn::MeshOffsetAttr::get(contextOp->getContext(), meshOffset[0],
                                  meshOffset[1]));
    builder.restoreInsertionPoint(currentInsertionPoint);
    return deviceOp;
  }

  void processMemReconfigEdges(
      const llvm::DenseMap<Edge, MemReconfigEntry> &memReconfigEntryMap) {

    // Insert memory reconfig ops here based on results of memory layout
    // analysis.
    //
    for (const auto &[edge, memReconfigEntry] : memReconfigEntryMap) {
      TTMLIR_TRACE(ttmlir::LogComponent::Optimizer,
                   "Processing mem reconfig edge: {}", edge);

      Operation *producerOp = edge.producerOp;
      Operation *consumerOp = edge.consumerOp;

      const bool overridenReconfig = memReconfigEntry.hasOverridenReconfig();
      assert(overridenReconfig ||
             memReconfigEntry.hasSelectedReshardOutputConfigBitIndex());

      // If there's no producer, tensor is defined by the consumer op input
      // operand.
      RankedTensorType producerOpTensorType =
          producerOp
              ? mlir::cast<RankedTensorType>(producerOp->getResult(0).getType())
              : mlir::cast<RankedTensorType>(
                    consumerOp->getOperand(edge.operandIndex).getType());

      llvm::ArrayRef<int64_t> producerOpTensorShape =
          producerOpTensorType.getShape();

      // Pick first layout from the list of layouts or consumer output layout if
      // this is the case of an override.
      TTNNLayoutAttr producerOpLayout =
          overridenReconfig
              ? mlir::cast<TTNNLayoutAttr>(
                    mlir::cast<RankedTensorType>(
                        consumerOp->getResult(0).getType())
                        .getEncoding())
              : memReconfigEntry.reshardOutputConfigMap
                    .at(memReconfigEntry
                            .getSelectedReshardOutputConfigBitIndex())
                    .front()
                    .outputLayout;

      // TODO(nobradovic): Match memory space and layout of consumer op.
      // This actually needs to be properly resolved based on op type, output
      // layout and other inputs.
      //
      RankedTensorType newTensorType = RankedTensorType::get(
          producerOpTensorShape, producerOpTensorType.getElementType(),
          producerOpLayout);

      MemoryConfigAttr outputMemConfigAttr = MemoryConfigAttr::get(
          consumerOp->getContext(),
          BufferTypeAttr::get(consumerOp->getContext(),
                              producerOpLayout.getBufferType()),
          ShardSpecAttr::get(consumerOp->getContext(),
                             ShapeAttr::get(consumerOp->getContext(),
                                            producerOpLayout.getShardShape())),
          producerOpLayout.getMemLayout());

      // If producerOp is a toLayoutOp, adjust its output layout(update
      // inplace) to reflect consumerOp's output layout. If producerOp is not a
      // toLayoutOp, insert a toLayoutOp in between producerOp
      // and consumerOp.
      //
      if (isa_and_nonnull<ToLayoutOp>(producerOp)) {
        ToLayoutOp toLayoutOp = llvm::cast<ToLayoutOp>(producerOp);
        toLayoutOp.setMemoryConfigAttr(outputMemConfigAttr);
        toLayoutOp.getResult().setType(newTensorType);
      } else {
        OpBuilder builder(consumerOp);
        Location loc = ttmlir::utils::appendLocationSuffix(consumerOp->getLoc(),
                                                           "_mem_reconfig");
        Operation *memoryReconfigOp = builder.create<ToLayoutOp>(
            loc,
            newTensorType,                             // output type
            consumerOp->getOperand(edge.operandIndex), // input value
            LayoutAttr::get(consumerOp->getContext(),
                            producerOpLayout.getLayout()),
            DataTypeAttr::get(consumerOp->getContext(),
                              producerOpLayout.getDataType()),
            outputMemConfigAttr, getOrCreateDeviceOpValue(consumerOp, builder));

        consumerOp->setOperand(edge.operandIndex,
                               memoryReconfigOp->getResult(0));
        TTMLIR_TRACE(ttmlir::LogComponent::Optimizer,
                     "Inserted memory reconfig op: {}",
                     mlir::cast<ToLayoutOp>(memoryReconfigOp));
      }
    }
  }

  void processSpillOps(const std::vector<Operation *> &spillToDramOps) {
    for (Operation *op : spillToDramOps) {
      TTMLIR_TRACE(ttmlir::LogComponent::Optimizer, "Processing spill op: {}",
                   op->getName());
      RankedTensorType tensorType =
          mlir::cast<RankedTensorType>(op->getResult(0).getType());
      llvm::ArrayRef<int64_t> tensorShape = tensorType.getShape();
      TTNNLayoutAttr layoutAttr =
          mlir::cast<TTNNLayoutAttr>(tensorType.getEncoding());

      // Create a new tensor type with DRAM layout.
      TTNNLayoutAttr dramLayout =
          layoutAttr.withBufferType(BufferType::DRAM)
              .withMemoryLayout(TensorMemoryLayout::Interleaved);
      RankedTensorType newTensorType = RankedTensorType::get(
          tensorShape, tensorType.getElementType(), dramLayout);

      // Create a ToLayoutOp with the new DRAM layout.
      OpBuilder builder(op->getContext());
      DataTypeAttr dataType =
          DataTypeAttr::get(op->getContext(), dramLayout.getDataType());
      LayoutAttr newLayout =
          LayoutAttr::get(op->getContext(), dramLayout.getLayout());
      MemoryConfigAttr memConfigAttr = MemoryConfigAttr::get(
          op->getContext(),
          BufferTypeAttr::get(op->getContext(), BufferType::DRAM),
          ShardSpecAttr::get(
              op->getContext(),
              ShapeAttr::get(op->getContext(), dramLayout.getShardShape())),
          dramLayout.getMemLayout());

      builder.setInsertionPointAfter(op);
      Location loc =
          ttmlir::utils::appendLocationSuffix(op->getLoc(), "_spill");
      Operation *toLayoutOp = builder.create<ToLayoutOp>(
          loc, newTensorType, op->getResult(0), newLayout, dataType,
          memConfigAttr, getOrCreateDeviceOpValue(op, builder));
      TTMLIR_TRACE(ttmlir::LogComponent::Optimizer,
                   "Inserted spill to DRAM prevLayout: {}\nnewLayout: {}",
                   layoutAttr, newLayout);

      for (auto &use : op->getResult(0).getUses()) {
        if (use.getOwner() != toLayoutOp) {
          use.getOwner()->setOperand(use.getOperandNumber(),
                                     toLayoutOp->getResult(0));
        }
      }
    }
  }

  // Trace all possible layouts for debugging
  static void
  tracePossibleLayouts(const TensorTypeLayoutsMap &allPossibleLayouts) {
    if (!llvm::DebugFlag || !isLogLevelEnabled(ttmlir::LogLevel::Trace)) {
      return;
    }
    for (auto &[tensorType, layouts] : allPossibleLayouts) {
      for (auto &[scalarType, layoutsByDataLayout] : layouts) {
        for (size_t dataLayoutIdx = 0;
             dataLayoutIdx < static_cast<size_t>(TensorPageLayout::kNumValues);
             ++dataLayoutIdx) {
          // Get data layout enum name for debugging
          std::string dataLayoutName =
              dataLayoutIdx == static_cast<size_t>(TensorPageLayout::Tiled)
                  ? "Tiled"
                  : "RowMajor";

          TTMLIR_TRACE(ttmlir::LogComponent::Optimizer, "data layout {}",
                       dataLayoutName);

          for (size_t memLayoutIdx = 0;
               memLayoutIdx <
               static_cast<size_t>(TensorMemoryLayoutIndex::kNumValues);
               ++memLayoutIdx) {
            std::string memLayoutName =
                memLayoutIdx == static_cast<size_t>(
                                    TensorMemoryLayoutIndex::Interleaved)
                    ? "Interleaved"
                    : "Sharded";

            auto &layouts = layoutsByDataLayout[dataLayoutIdx][memLayoutIdx];
            for (const TTNNLayoutAttr &layout : layouts) {
              (void)layout;
              TTMLIR_TRACE(ttmlir::LogComponent::Optimizer,
                           "Tensor type {0}, scalar type {1}, data layout "
                           "{2}, memory layout "
                           "{3} has layout {4}",
                           tensorType, scalarType, dataLayoutName,
                           memLayoutName, layout);
            }
          }
        }
      }
    }
  }
};

} // namespace mlir::tt::ttnn
