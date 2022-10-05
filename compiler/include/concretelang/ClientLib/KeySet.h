// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/main/LICENSE.txt
// for license information.

#ifndef CONCRETELANG_CLIENTLIB_KEYSET_H_
#define CONCRETELANG_CLIENTLIB_KEYSET_H_

#include <memory>

#include "boost/outcome.h"

#include "concrete-core-ffi.h"
#include "concretelang/Runtime/DFRuntime.hpp"
#include "concretelang/Runtime/context.h"

#include "concretelang/ClientLib/ClientParameters.h"
#include "concretelang/ClientLib/EvaluationKeys.h"
#include "concretelang/ClientLib/KeySetCache.h"
#include "concretelang/Common/Error.h"

namespace concretelang {
namespace clientlib {

using concretelang::error::StringError;
using RuntimeContext = mlir::concretelang::RuntimeContext;

class KeySet {
public:
  KeySet();
  ~KeySet();
  KeySet(KeySet &other) = delete;

  /// allocate a KeySet according the ClientParameters.
  static outcome::checked<std::unique_ptr<KeySet>, StringError>
  generate(ClientParameters &params, uint64_t seed_msb, uint64_t seed_lsb);

  /// Returns the ClientParameters associated with the KeySet.
  ClientParameters clientParameters() { return _clientParameters; }

  // isInputEncrypted return true if the input at the given pos is encrypted.
  bool isInputEncrypted(size_t pos);

  /// getInputLweSecretKeyParam returns the parameters of the lwe secret key for
  /// the input at the given `pos`.
  /// The input must be encrupted
  LweSecretKeyParam getInputLweSecretKeyParam(size_t pos) {
    auto gate = inputGate(pos);
    auto inputSk = this->secretKeys.find(gate.encryption->secretKeyID);
    return inputSk->second.first;
  }

  /// getOutputLweSecretKeyParam returns the parameters of the lwe secret key
  /// for the given output.
  LweSecretKeyParam getOutputLweSecretKeyParam(size_t pos) {
    auto gate = outputGate(pos);
    auto outputSk = this->secretKeys.find(gate.encryption->secretKeyID);
    return outputSk->second.first;
  }

  /// allocate a lwe ciphertext buffer for the argument at argPos, set the size
  /// of the allocated buffer.
  outcome::checked<void, StringError>
  allocate_lwe(size_t argPos, uint64_t **ciphertext, uint64_t &size);

  /// encrypt the input to the ciphertext for the argument at argPos.
  outcome::checked<void, StringError>
  encrypt_lwe(size_t argPos, uint64_t *ciphertext, uint64_t input);

  /// isOuputEncrypted return true if the output at the given pos is encrypted.
  bool isOutputEncrypted(size_t pos);

  /// decrypt the ciphertext to the output for the argument at argPos.
  outcome::checked<void, StringError>
  decrypt_lwe(size_t argPos, uint64_t *ciphertext, uint64_t &output);

  size_t numInputs() { return inputs.size(); }
  size_t numOutputs() { return outputs.size(); }

  CircuitGate inputGate(size_t pos) { return std::get<0>(inputs[pos]); }
  CircuitGate outputGate(size_t pos) { return std::get<0>(outputs[pos]); }

  RuntimeContext runtimeContext() {
    RuntimeContext context;
    context.evaluationKeys = this->evaluationKeys();
    return context;
  }

  EvaluationKeys evaluationKeys() {
    if (this->bootstrapKeys.empty() && this->keyswitchKeys.empty()) {
      return EvaluationKeys();
    }
    auto kskIt = this->keyswitchKeys.find(clientlib::KEYSWITCH_KEY);
    auto bskIt = this->bootstrapKeys.find(clientlib::BOOTSTRAP_KEY);
    auto fpkskIt = this->packingKeys.find("fpksk_v0");
    if (kskIt != this->keyswitchKeys.end() &&
        bskIt != this->bootstrapKeys.end()) {
      auto sharedKsk = std::get<1>(kskIt->second);
      auto sharedBsk = std::get<1>(bskIt->second);
      auto sharedFpksk = fpkskIt == this->packingKeys.end()
                             ? std::make_shared<PackingKeyswitchKey>(nullptr)
                             : std::get<1>(fpkskIt->second);
      return EvaluationKeys(sharedKsk, sharedBsk, sharedFpksk);
    }
    assert(!mlir::concretelang::dfr::_dfr_is_root_node() &&
           "Evaluation keys missing in KeySet (on root node).");
    return EvaluationKeys();
  }

  const std::map<LweSecretKeyID, std::pair<LweSecretKeyParam, LweSecretKey64 *>>
      &getSecretKeys();

  const std::map<LweSecretKeyID,
                 std::pair<BootstrapKeyParam, std::shared_ptr<LweBootstrapKey>>>
      &getBootstrapKeys();

  const std::map<LweSecretKeyID,
                 std::pair<KeyswitchKeyParam, std::shared_ptr<LweKeyswitchKey>>>
      &getKeyswitchKeys();

  const std::map<
      LweSecretKeyID,
      std::pair<PackingKeySwitchParam, std::shared_ptr<PackingKeyswitchKey>>> &
  getPackingKeys();

protected:
  outcome::checked<void, StringError>
  generateSecretKey(LweSecretKeyID id, LweSecretKeyParam param);

  outcome::checked<void, StringError>
  generateBootstrapKey(BootstrapKeyID id, BootstrapKeyParam param);

  outcome::checked<void, StringError>
  generateKeyswitchKey(KeyswitchKeyID id, KeyswitchKeyParam param);

  outcome::checked<void, StringError>
  generatePackingKey(PackingKeySwitchID id, PackingKeySwitchParam param);

  outcome::checked<void, StringError>
  generateKeysFromParams(ClientParameters &params, uint64_t seed_msb,
                         uint64_t seed_lsb);

  outcome::checked<void, StringError>
  setupEncryptionMaterial(ClientParameters &params, uint64_t seed_msb,
                          uint64_t seed_lsb);

  friend class KeySetCache;

private:
  DefaultEngine *engine;
  DefaultParallelEngine *par_engine;
  std::map<LweSecretKeyID, std::pair<LweSecretKeyParam, LweSecretKey64 *>>
      secretKeys;
  std::map<LweSecretKeyID,
           std::pair<BootstrapKeyParam, std::shared_ptr<LweBootstrapKey>>>
      bootstrapKeys;
  std::map<LweSecretKeyID,
           std::pair<KeyswitchKeyParam, std::shared_ptr<LweKeyswitchKey>>>
      keyswitchKeys;
  std::map<LweSecretKeyID, std::pair<PackingKeySwitchParam,
                                     std::shared_ptr<PackingKeyswitchKey>>>
      packingKeys;
  std::vector<std::tuple<CircuitGate, LweSecretKeyParam, LweSecretKey64 *>>
      inputs;
  std::vector<std::tuple<CircuitGate, LweSecretKeyParam, LweSecretKey64 *>>
      outputs;

  void setKeys(
      std::map<LweSecretKeyID, std::pair<LweSecretKeyParam, LweSecretKey64 *>>
          secretKeys,
      std::map<LweSecretKeyID,
               std::pair<BootstrapKeyParam, std::shared_ptr<LweBootstrapKey>>>
          bootstrapKeys,
      std::map<LweSecretKeyID,
               std::pair<KeyswitchKeyParam, std::shared_ptr<LweKeyswitchKey>>>
          keyswitchKeys,
      std::map<LweSecretKeyID, std::pair<PackingKeySwitchParam,
                                         std::shared_ptr<PackingKeyswitchKey>>>
          packingKeys);

  clientlib::ClientParameters _clientParameters;
};

} // namespace clientlib
} // namespace concretelang

#endif
