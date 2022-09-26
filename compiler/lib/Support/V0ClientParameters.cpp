// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/main/LICENSE.txt
// for license information.
#include <map>

#include <llvm/ADT/Optional.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Error.h>

#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include "concretelang/ClientLib/ClientParameters.h"
#include "concretelang/Conversion/Utils/GlobalFHEContext.h"
#include "concretelang/Dialect/Concrete/IR/ConcreteTypes.h"
#include "concretelang/Support/Error.h"
#include "concretelang/Support/V0Curves.h"

namespace mlir {
namespace concretelang {

namespace clientlib = ::concretelang::clientlib;
using ::concretelang::clientlib::CircuitGate;
using ::concretelang::clientlib::ClientParameters;
using ::concretelang::clientlib::Encoding;
using ::concretelang::clientlib::EncryptionGate;
using ::concretelang::clientlib::LweSecretKeyID;
using ::concretelang::clientlib::Precision;
using ::concretelang::clientlib::Variance;

const auto securityLevel = SECURITY_LEVEL_128;
const auto keyFormat = KEY_FORMAT_BINARY;
const auto v0Curve = getV0Curves(securityLevel, keyFormat);

/// For the v0 the secretKeyID and precision are the same for all gates.
llvm::Expected<CircuitGate> gateFromMLIRType(LweSecretKeyID secretKeyID,
                                             Variance variance,
                                             mlir::Type type) {
  if (type.isIntOrIndex()) {
    // TODO - The index type is dependant of the target architecture, so
    // actually we assume we target only 64 bits, we need to have some the size
    // of the word of the target system.
    size_t width = 64;
    if (!type.isIndex()) {
      width = type.getIntOrFloatBitWidth();
    }
    return CircuitGate{
        /*.encryption = */ llvm::None,
        /*.shape = */
        {
            /*.width = */ width,
            /*.dimensions = */ std::vector<int64_t>(),
            /*.size = */ 0,
        },
    };
  }
  if (auto lweTy = type.dyn_cast_or_null<
                   mlir::concretelang::Concrete::LweCiphertextType>()) {
    return CircuitGate{
        /* .encryption = */ llvm::Optional<EncryptionGate>({
            /* .secretKeyID = */ secretKeyID,
            /* .variance = */ variance,
            /* .encoding = */
            {
                /* .precision = */ lweTy.getP(),
                /* .crt = */ lweTy.getCrtDecomposition().vec(),
            },
        }),
        /*.shape = */
        {
            /*.width = */ lweTy.getP(),
            /*.dimensions = */ std::vector<int64_t>(),
            /*.size = */ 0,
        },
    };
  }
  auto tensor = type.dyn_cast_or_null<mlir::RankedTensorType>();
  if (tensor != nullptr) {
    auto gate =
        gateFromMLIRType(secretKeyID, variance, tensor.getElementType());
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    gate->shape.dimensions = tensor.getShape().vec();
    gate->shape.size = 1;
    for (auto dimSize : gate->shape.dimensions) {
      gate->shape.size *= dimSize;
    }
    return gate;
  }
  return llvm::make_error<llvm::StringError>(
      "cannot convert MLIR type to shape", llvm::inconvertibleErrorCode());
}

llvm::Expected<ClientParameters>
createClientParametersForV0(V0FHEContext fheContext,
                            llvm::StringRef functionName,
                            mlir::ModuleOp module) {
  auto v0Param = fheContext.parameter;
  Variance inputVariance =
      v0Curve->getVariance(1, v0Param.getNBigLweDimension(), 64);

  Variance bootstrapKeyVariance = v0Curve->getVariance(
      v0Param.glweDimension, v0Param.getPolynomialSize(), 64);
  Variance keyswitchKeyVariance = v0Curve->getVariance(1, v0Param.nSmall, 64);
  // Static client parameters from global parameters for v0
  ClientParameters c;
  c.secretKeys = {
      {clientlib::BIG_KEY, {/*.size = */ v0Param.getNBigLweDimension()}},
  };
  bool has_small_key = v0Param.nSmall != 0;
  bool has_bootstrap = v0Param.brLevel != 0;
  if (has_small_key) {
    c.secretKeys.insert({clientlib::SMALL_KEY, {/*.size = */ v0Param.nSmall}});
  }
  if (has_bootstrap) {
    auto inputKey = (has_small_key) ? clientlib::SMALL_KEY : clientlib::BIG_KEY;
    c.bootstrapKeys = {
        {
            clientlib::BOOTSTRAP_KEY,
            {
                /*.inputSecretKeyID = */ inputKey,
                /*.outputSecretKeyID = */ clientlib::BIG_KEY,
                /*.level = */ v0Param.brLevel,
                /*.baseLog = */ v0Param.brLogBase,
                /*.glweDimension = */ v0Param.glweDimension,
                /*.variance = */ bootstrapKeyVariance,
            },
        },
    };
  }
  if (has_small_key) {
    c.keyswitchKeys = {
        {
            clientlib::KEYSWITCH_KEY,
            {
                /*.inputSecretKeyID = */ clientlib::BIG_KEY,
                /*.outputSecretKeyID = */ clientlib::SMALL_KEY,
                /*.level = */ v0Param.ksLevel,
                /*.baseLog = */ v0Param.ksLogBase,
                /*.variance = */ keyswitchKeyVariance,
            },
        },
    };
  }
  c.functionName = (std::string)functionName;
  // Find the input function
  auto rangeOps = module.getOps<mlir::func::FuncOp>();
  auto funcOp = llvm::find_if(rangeOps, [&](mlir::func::FuncOp op) {
    return op.getName() == functionName;
  });
  if (funcOp == rangeOps.end()) {
    return StreamStringError(
               "cannot find the function for generate client parameters: ")
           << functionName;
  }

  // Create input and output circuit gate parameters
  auto funcType = (*funcOp).getFunctionType();

  auto inputs = funcType.getInputs();

  bool hasContext =
      inputs.empty()
          ? false
          : inputs.back().isa<mlir::concretelang::Concrete::ContextType>();

  auto gateFromType = [&](mlir::Type ty) {
    return gateFromMLIRType(clientlib::BIG_KEY, inputVariance, ty);
  };
  for (auto inType = funcType.getInputs().begin();
       inType < funcType.getInputs().end() - hasContext; inType++) {
    auto gate = gateFromType(*inType);
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    c.inputs.push_back(gate.get());
  }
  for (auto outType : funcType.getResults()) {
    auto gate = gateFromType(outType);
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    c.outputs.push_back(gate.get());
  }
  return c;
}

} // namespace concretelang
} // namespace mlir
