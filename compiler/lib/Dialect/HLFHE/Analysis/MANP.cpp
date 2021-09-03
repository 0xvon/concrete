#include <zamalang/Dialect/HLFHE/Analysis/MANP.h>
#include <zamalang/Dialect/HLFHE/IR/HLFHEDialect.h>
#include <zamalang/Dialect/HLFHE/IR/HLFHEOps.h>
#include <zamalang/Dialect/HLFHE/IR/HLFHETypes.h>

#include <limits>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallString.h>
#include <mlir/Analysis/DataFlowAnalysis.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/IR/Attributes.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Support/LogicalResult.h>

#define GEN_PASS_CLASSES
#include <zamalang/Dialect/HLFHE/Analysis/MANP.h.inc>

namespace mlir {
namespace zamalang {
namespace {
// The `MANPLatticeValue` represents the squared Minimal Arithmetic
// Noise Padding for an operation using the squared 2-norm of an
// equivalent dot operation. This can either be an actual value if the
// values for its predecessors have been calculated beforehand or an
// unknown value otherwise.
struct MANPLatticeValue {
  MANPLatticeValue(llvm::Optional<llvm::APInt> manp = {}) : manp(manp) {}

  static MANPLatticeValue getPessimisticValueState(mlir::MLIRContext *context) {
    return MANPLatticeValue();
  }

  static MANPLatticeValue getPessimisticValueState(mlir::Value value) {
    // Function arguments are assumed to require a Minimal Arithmetic
    // Noise Padding with a 2-norm of 1.
    //
    // TODO: Provide a mechanism to propagate Minimal Arithmetic Noise
    // Padding across function calls.
    if (value.isa<mlir::BlockArgument>() &&
        (value.getType().isa<mlir::zamalang::HLFHE::EncryptedIntegerType>() ||
         (value.getType().isa<mlir::TensorType>() &&
          value.getType()
              .cast<mlir::TensorType>()
              .getElementType()
              .isa<mlir::zamalang::HLFHE::EncryptedIntegerType>()))) {
      return MANPLatticeValue(llvm::APInt{1, 1, false});
    } else {
      // All other operations have an unknown Minimal Arithmetic Noise
      // Padding until an value for all predecessors has been
      // calculated.
      return MANPLatticeValue();
    }
  }

  bool operator==(const MANPLatticeValue &rhs) const {
    return this->manp == rhs.manp;
  }

  // Required by `mlir::LatticeElement::join()`, but should never be
  // invoked, as `MANPAnalysis::visitOperation()` takes care of
  // combining the squared Minimal Arithmetic Noise Padding of
  // operands into the Minimal Arithmetic Noise Padding of the result.
  static MANPLatticeValue join(const MANPLatticeValue &lhs,
                               const MANPLatticeValue &rhs) {
    assert(false && "Minimal Arithmetic Noise Padding values can only be "
                    "combined sensibly when the combining operation is known");
    return MANPLatticeValue{};
  }

  llvm::Optional<llvm::APInt> getMANP() { return manp; }

protected:
  llvm::Optional<llvm::APInt> manp;
};

// Checks if `lhs` is less than `rhs`, where both values are assumed
// to be positive. The bit width of the smaller `APInt` is extended
// before comparison via `APInt::ult`.
static bool APIntWidthExtendULT(const llvm::APInt &lhs,
                                const llvm::APInt &rhs) {
  if (lhs.getBitWidth() < rhs.getBitWidth())
    return lhs.zext(rhs.getBitWidth()).ult(rhs);
  else if (lhs.getBitWidth() > rhs.getBitWidth())
    return lhs.ult(rhs.zext(lhs.getBitWidth()));
  else
    return lhs.ult(rhs);
}

// Adds two `APInt` values, where both values are assumed to be
// positive. The bit width of the operands is extended in order to
// guarantee that the sum fits into the resulting `APInt`.
static llvm::APInt APIntWidthExtendUAdd(const llvm::APInt &lhs,
                                        const llvm::APInt &rhs) {
  unsigned maxBits = std::max(lhs.getBitWidth(), rhs.getBitWidth());

  // Make sure the required number of bits can be represented by the
  // `unsigned` argument of `zext`.
  assert(std::numeric_limits<unsigned>::max() - maxBits > 1);

  unsigned targetWidth = maxBits + 1;
  return lhs.zext(targetWidth) + rhs.zext(targetWidth);
}

// Multiplies two `APInt` values, where both values are assumed to be
// positive. The bit width of the operands is extended in order to
// guarantee that the product fits into the resulting `APInt`.
static llvm::APInt APIntWidthExtendUMul(const llvm::APInt &lhs,
                                        const llvm::APInt &rhs) {
  // Make sure the required number of bits can be represented by the
  // `unsigned` argument of `zext`.
  assert(std::numeric_limits<unsigned>::max() -
                 std::max(lhs.getBitWidth(), rhs.getBitWidth()) >
             std::min(lhs.getBitWidth(), rhs.getBitWidth()) &&
         "Required number of bits cannot be represented with an APInt");

  unsigned targetWidth = lhs.getBitWidth() + rhs.getBitWidth();
  return lhs.zext(targetWidth) * rhs.zext(targetWidth);
}

// Calculates the square of `i`. The bit width `i` is extended in
// order to guarantee that the product fits into the resulting
// `APInt`.
static llvm::APInt APIntWidthExtendUSq(const llvm::APInt &i) {
  // Make sure the required number of bits can be represented by the
  // `unsigned` argument of `zext`.
  assert(i.getBitWidth() < std::numeric_limits<unsigned>::max() / 2 &&
         "Required number of bits cannot be represented with an APInt");

  llvm::APInt ie = i.zext(2 * i.getBitWidth());

  return ie * ie;
}

// Calculates the square root of `i` and rounds it to the next highest
// integer value (i.e., the square of the result is guaranteed to be
// greater or equal to `i`).
static llvm::APInt APIntCeilSqrt(const llvm::APInt &i) {
  llvm::APInt res = i.sqrt();
  llvm::APInt resSq = APIntWidthExtendUSq(res);

  if (APIntWidthExtendULT(resSq, i))
    return APIntWidthExtendUAdd(res, llvm::APInt{1, 1, false});
  else
    return res;
}

// Returns a string representation of `i` assuming that `i` is an
// unsigned value.
static std::string APIntToStringValUnsigned(const llvm::APInt &i) {
  llvm::SmallString<32> s;
  i.toStringUnsigned(s);
  return std::string(s.c_str());
}

// Calculates the square of the 2-norm of a tensor initialized with a
// dense matrix of constant, signless integers. Aborts if the value
// type or initialization of of `cstOp` is incorrect.
static llvm::APInt denseCstTensorNorm2Sq(mlir::ConstantOp cstOp) {
  mlir::DenseIntElementsAttr denseVals =
      cstOp->getAttrOfType<mlir::DenseIntElementsAttr>("value");

  assert(denseVals && cstOp.getType().isa<mlir::TensorType>() &&
         "Constant must be a tensor initialized with `dense`");

  mlir::TensorType tensorType = cstOp.getType().cast<mlir::TensorType>();

  assert(tensorType.getElementType().isSignlessInteger() &&
         "Can only handle tensors with signless integer elements");

  mlir::IntegerType elementType =
      tensorType.getElementType().cast<mlir::IntegerType>();

  llvm::APInt accu{1, 0, false};

  for (llvm::APInt val : denseVals.getIntValues()) {
    llvm::APInt valSq = APIntWidthExtendUSq(val);
    accu = APIntWidthExtendUAdd(accu, valSq);
  }

  return accu;
}

// Calculates (T)ceil(log2f(v))
// TODO: Replace with some fancy bit twiddling hack
template <typename T> static T ceilLog2(const T v) {
  T tmp = v;
  T log2 = 0;

  while (tmp >>= 1)
    log2++;

  // If more than MSB set, round to next highest power of 2
  if (v & ~((T)1 << log2))
    log2 += 1;

  return log2;
}

// Calculates the square of the 2-norm of a 1D tensor of signless
// integers by conservatively assuming that the dynamic values are the
// maximum for the integer width. Aborts if the tensor type `tTy` is
// incorrect.
static llvm::APInt denseDynTensorNorm2Sq(mlir::TensorType tTy) {
  assert(tTy && tTy.getElementType().isSignlessInteger() &&
         tTy.hasStaticShape() && tTy.getRank() == 1 &&
         "Plaintext operand must be a statically shaped 1D tensor of integers");

  // Make sure the log2 of the number of elements fits into an
  // unsigned
  assert(std::numeric_limits<unsigned>::max() > 8 * sizeof(uint64_t));

  unsigned elWidth = tTy.getElementTypeBitWidth();

  llvm::APInt maxVal = APInt::getMaxValue(elWidth);
  llvm::APInt maxValSq = APIntWidthExtendUSq(maxVal);

  // Calculate number of bits for APInt to store number of elements
  uint64_t nElts = (uint64_t)tTy.getNumElements();
  assert(std::numeric_limits<int64_t>::max() - nElts > 1);
  unsigned nEltsBits = (unsigned)ceilLog2(nElts + 1);

  llvm::APInt nEltsAP{nEltsBits, nElts, false};

  return APIntWidthExtendUMul(maxValSq, nEltsAP);
}

// Calculates the squared Minimal Arithmetic Noise Padding of an
// `HLFHE.dot_eint_int` operation.
static llvm::APInt getSqMANP(
    mlir::zamalang::HLFHE::Dot op,
    llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operandMANPs) {
  assert(op->getOpOperand(0).get().isa<mlir::BlockArgument>() &&
         "Only dot operations with tensors that are function arguments are "
         "currently supported");

  mlir::ConstantOp cstOp = llvm::dyn_cast_or_null<mlir::ConstantOp>(
      op->getOpOperand(1).get().getDefiningOp());

  if (cstOp) {
    // Dot product between a vector of encrypted integers and a vector
    // of plaintext constants -> return 2-norm of constant vector
    return denseCstTensorNorm2Sq(cstOp);
  } else {
    // Dot product between a vector of encrypted integers and a vector
    // of dynamic plaintext values -> conservatively assume that all
    // the values are the maximum possible value for the integer's
    // width
    mlir::TensorType tTy = op->getOpOperand(1)
                               .get()
                               .getType()
                               .dyn_cast_or_null<mlir::TensorType>();

    return denseDynTensorNorm2Sq(tTy);
  }
}

// Returns the squared 2-norm for a dynamic integer by conservatively
// assuming that the integer's value is the maximum for the integer
// width.
static llvm::APInt conservativeIntNorm2Sq(mlir::Type t) {
  assert(t.isSignlessInteger() && "Type must be a signless integer type");
  assert(std::numeric_limits<unsigned>::max() - t.getIntOrFloatBitWidth() > 1);

  llvm::APInt maxVal{t.getIntOrFloatBitWidth() + 1, 1, false};
  maxVal <<= t.getIntOrFloatBitWidth();
  return APIntWidthExtendUSq(maxVal);
}

// Calculates the squared Minimal Arithmetic Noise Padding of an
// `HLFHE.add_eint_int` operation.
static llvm::APInt getSqMANP(
    mlir::zamalang::HLFHE::AddEintIntOp op,
    llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operandMANPs) {
  mlir::Type iTy = op->getOpOperand(1).get().getType();

  assert(iTy.isSignlessInteger() &&
         "Only additions with signless integers are currently allowed");

  assert(
      operandMANPs.size() == 2 &&
      operandMANPs[0]->getValue().getMANP().hasValue() &&
      "Missing squared Minimal Arithmetic Noise Padding for encrypted operand");

  mlir::ConstantOp cstOp = llvm::dyn_cast_or_null<mlir::ConstantOp>(
      op->getOpOperand(1).get().getDefiningOp());

  llvm::APInt eNorm = operandMANPs[0]->getValue().getMANP().getValue();
  llvm::APInt sqNorm;

  if (cstOp) {
    // For a constant operand use actual constant to calculate 2-norm
    mlir::IntegerAttr attr = cstOp->getAttrOfType<mlir::IntegerAttr>("value");
    sqNorm = APIntWidthExtendUSq(attr.getValue());
  } else {
    // For a dynamic operand conservatively assume that the value is
    // the maximum for the integer width
    sqNorm = conservativeIntNorm2Sq(iTy);
  }

  return APIntWidthExtendUAdd(sqNorm, eNorm);
}

// Calculates the squared Minimal Arithmetic Noise Padding of a dot operation
// that is equivalent to an `HLFHE.add_eint` operation.
static llvm::APInt getSqMANP(
    mlir::zamalang::HLFHE::AddEintOp op,
    llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operandMANPs) {
  assert(operandMANPs.size() == 2 &&
         operandMANPs[0]->getValue().getMANP().hasValue() &&
         operandMANPs[1]->getValue().getMANP().hasValue() &&
         "Missing squared Minimal Arithmetic Noise Padding for encrypted "
         "operands");

  llvm::APInt a = operandMANPs[0]->getValue().getMANP().getValue();
  llvm::APInt b = operandMANPs[1]->getValue().getMANP().getValue();

  return APIntWidthExtendUAdd(a, b);
}

// Calculates the squared Minimal Arithmetic Noise Padding of a dot operation
// that is equivalent to an `HLFHE.sub_int_eint` operation.
static llvm::APInt getSqMANP(
    mlir::zamalang::HLFHE::SubIntEintOp op,
    llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operandMANPs) {
  mlir::Type iTy = op->getOpOperand(0).get().getType();

  assert(iTy.isSignlessInteger() &&
         "Only subtractions with signless integers are currently allowed");

  assert(
      operandMANPs.size() == 2 &&
      operandMANPs[1]->getValue().getMANP().hasValue() &&
      "Missing squared Minimal Arithmetic Noise Padding for encrypted operand");

  llvm::APInt eNorm = operandMANPs[1]->getValue().getMANP().getValue();
  llvm::APInt sqNorm;

  mlir::ConstantOp cstOp = llvm::dyn_cast_or_null<mlir::ConstantOp>(
      op->getOpOperand(0).get().getDefiningOp());

  if (cstOp) {
    // For constant plaintext operands simply use the constant value
    mlir::IntegerAttr attr = cstOp->getAttrOfType<mlir::IntegerAttr>("value");
    sqNorm = APIntWidthExtendUSq(attr.getValue());
  } else {
    // For dynamic plaintext operands conservatively assume that the integer has
    // its maximum possible value
    sqNorm = conservativeIntNorm2Sq(iTy);
  }
  return APIntWidthExtendUAdd(sqNorm, eNorm);
}

// Calculates the squared Minimal Arithmetic Noise Padding of a dot operation
// that is equivalent to an `HLFHE.mul_eint_int` operation.
static llvm::APInt getSqMANP(
    mlir::zamalang::HLFHE::MulEintIntOp op,
    llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operandMANPs) {
  mlir::Type iTy = op->getOpOperand(1).get().getType();

  assert(iTy.isSignlessInteger() &&
         "Only multiplications with signless integers are currently allowed");

  assert(
      operandMANPs.size() == 2 &&
      operandMANPs[0]->getValue().getMANP().hasValue() &&
      "Missing squared Minimal Arithmetic Noise Padding for encrypted operand");

  mlir::ConstantOp cstOp = llvm::dyn_cast_or_null<mlir::ConstantOp>(
      op->getOpOperand(1).get().getDefiningOp());

  llvm::APInt eNorm = operandMANPs[0]->getValue().getMANP().getValue();
  llvm::APInt sqNorm;

  if (cstOp) {
    // For a constant operand use actual constant to calculate 2-norm
    mlir::IntegerAttr attr = cstOp->getAttrOfType<mlir::IntegerAttr>("value");
    sqNorm = APIntWidthExtendUSq(attr.getValue());
  } else {
    // For a dynamic operand conservatively assume that the value is
    // the maximum for the integer width
    sqNorm = conservativeIntNorm2Sq(iTy);
  }

  return APIntWidthExtendUMul(sqNorm, eNorm);
}

struct MANPAnalysis : public mlir::ForwardDataFlowAnalysis<MANPLatticeValue> {
  using ForwardDataFlowAnalysis<MANPLatticeValue>::ForwardDataFlowAnalysis;
  MANPAnalysis(mlir::MLIRContext *ctx, bool debug)
      : debug(debug), mlir::ForwardDataFlowAnalysis<MANPLatticeValue>(ctx) {}

  ~MANPAnalysis() override = default;

  mlir::ChangeResult visitOperation(
      mlir::Operation *op,
      llvm::ArrayRef<mlir::LatticeElement<MANPLatticeValue> *> operands) final {
    mlir::LatticeElement<MANPLatticeValue> &latticeRes =
        getLatticeElement(op->getResult(0));
    bool isDummy = false;
    llvm::APInt norm2SqEquiv;

    if (auto dotOp = llvm::dyn_cast<mlir::zamalang::HLFHE::Dot>(op)) {
      norm2SqEquiv = getSqMANP(dotOp, operands);
    } else if (auto addEintIntOp =
                   llvm::dyn_cast<mlir::zamalang::HLFHE::AddEintIntOp>(op)) {
      norm2SqEquiv = getSqMANP(addEintIntOp, operands);
    } else if (auto addEintOp =
                   llvm::dyn_cast<mlir::zamalang::HLFHE::AddEintOp>(op)) {
      norm2SqEquiv = getSqMANP(addEintOp, operands);
    } else if (auto subIntEintOp =
                   llvm::dyn_cast<mlir::zamalang::HLFHE::SubIntEintOp>(op)) {
      norm2SqEquiv = getSqMANP(subIntEintOp, operands);
    } else if (auto mulEintIntOp =
                   llvm::dyn_cast<mlir::zamalang::HLFHE::MulEintIntOp>(op)) {
      norm2SqEquiv = getSqMANP(mulEintIntOp, operands);
    } else if (llvm::isa<mlir::zamalang::HLFHE::ZeroEintOp>(op) ||
               llvm::isa<mlir::zamalang::HLFHE::ApplyLookupTableEintOp>(op)) {
      norm2SqEquiv = llvm::APInt{1, 1, false};
    } else if (llvm::isa<mlir::ConstantOp>(op)) {
      isDummy = true;
    } else if (llvm::isa<mlir::zamalang::HLFHE::HLFHEDialect>(
                   *op->getDialect())) {
      op->emitError("Unsupported operation");
      assert(false && "Unsupported operation");
    } else {
      isDummy = true;
    }

    if (!isDummy) {
      latticeRes.join(MANPLatticeValue{norm2SqEquiv});
      latticeRes.markOptimisticFixpoint();

      llvm::APInt norm2Equiv = APIntCeilSqrt(norm2SqEquiv);

      op->setAttr("MANP",
                  mlir::IntegerAttr::get(
                      mlir::IntegerType::get(
                          op->getContext(), norm2Equiv.getBitWidth(),
                          mlir::IntegerType::SignednessSemantics::Unsigned),
                      norm2Equiv));

      if (debug) {
        op->emitRemark("Squared Minimal Arithmetic Noise Padding: ")
            << APIntToStringValUnsigned(norm2SqEquiv) << "\n";
      }
    } else {
      latticeRes.join(MANPLatticeValue{});
    }

    return mlir::ChangeResult::Change;
  }

private:
  bool debug;
};
} // namespace

namespace {
// For documentation see MANP.td
struct MANPPass : public MANPBase<MANPPass> {
  void runOnFunction() override {
    mlir::FuncOp func = getFunction();

    MANPAnalysis analysis(func->getContext(), debug);
    analysis.run(func);
  }
  MANPPass() = delete;
  MANPPass(bool debug) : debug(debug){};

protected:
  bool debug;
};
} // end anonymous namespace

// Create an instance of the Minimal Arithmetic Noise Padding analysis
// pass. If `debug` is true, for each operation, the pass emits a
// remark containing the squared Minimal Arithmetic Noise Padding of
// the equivalent dot operation.
std::unique_ptr<mlir::Pass> createMANPPass(bool debug) {
  return std::make_unique<MANPPass>(debug);
}
} // namespace zamalang
} // namespace mlir
