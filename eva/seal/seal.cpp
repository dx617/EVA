// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "eva/seal/seal.h"
#include "eva/common/program_traversal.h"
#include "eva/common/valuation.h"
#include "eva/seal/seal_executor.h"
#include "eva/util/logging.h"
#include "base64.h"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef EVA_USE_GALOIS
#include "eva/common/multicore_program_traversal.h"
#include "eva/util/galois.h"
#endif

using namespace std;

namespace eva {

SEALValuation SEALPublic::encrypt(const Valuation &inputs,
                                  const CKKSSignature &signature) {
  size_t slotCount = encoder.slot_count();
  if (slotCount < signature.vecSize) {
    throw runtime_error("Vector size cannot be larger than slot count");
  }
  if (slotCount % signature.vecSize != 0) {
    throw runtime_error("Vector size must exactly divide the slot count");
  }

  SEALValuation sealInputs(context);
  for (auto &in : inputs) {

    // With multicore sealInputs is initialized first, so that multiple threads
    // can be used to encode and encrypt values into it at the same time without
    // making structural changes.
#ifdef EVA_USE_GALOIS
    sealInputs[in.first] = {};
  }

  // Start a second parallel loop to encrypt inputs.
  GaloisGuard galois;
  galois::do_all(
      galois::iterate(inputs),
      [&](auto &in) {
#endif
        auto name = in.first;
        auto &v = in.second;
        auto vSize = v.size();
        // TODO remove this check
        if (vSize != signature.vecSize) {
          throw runtime_error("Input size does not match program vector size");
        }
        auto info = signature.inputs.at(name);

        auto ctxData = context.first_context_data();
        for (size_t i = 0; i < info.level; ++i) {
          ctxData = ctxData->next_context_data();
        }

        if (info.inputType == Type::Cipher || info.inputType == Type::Plain) {
          seal::Plaintext plain;

          if (vSize == 1) {
            encoder.encode(v[0], ctxData->parms_id(), pow(2.0, info.scale),
                           plain);
          } else {
            vector<double> vec(slotCount);
            assert(vSize <= slotCount);
            assert((slotCount % vSize) == 0);
            auto replicas = (slotCount / vSize);
            for (uint32_t r = 0; r < replicas; ++r) {
              for (uint64_t i = 0; i < vSize; ++i) {
                vec[(r * vSize) + i] = v[i];
              }
            }
            encoder.encode(vec, ctxData->parms_id(), pow(2.0, info.scale),
                           plain);
          }
          if (info.inputType == Type::Cipher) {
            seal::Ciphertext cipher;
            encryptor.encrypt(plain, cipher);
            sealInputs[name] = move(cipher);
          } else if (info.inputType == Type::Plain) {
            sealInputs[name] = move(plain);
          }
        } else {
          sealInputs[name] = std::shared_ptr<ConstantValue>(
              new DenseConstantValue(signature.vecSize, v));
        }
      }
#ifdef EVA_USE_GALOIS
      // Finish the parallel loop if using multicore support
      ,
      galois::no_stats(), galois::loopname("EncryptInputs"));
#endif

  return sealInputs;
}

SEALValuation SEALPublic::recoverEncrypted(const std::unordered_map<std::string, std::string> &encodedEncryptedInputs,
                                           const CKKSSignature &signature) {
  SEALValuation sealInputs(context);

  for (auto &in : encodedEncryptedInputs) {
    auto name = in.first;
    auto &v = in.second;
    auto info = signature.inputs.at(name);
    if (info.inputType == Type::Cipher) {
      seal::Ciphertext cipher;

      std::string decoded = b64decode(v);
      std::istringstream is(decoded);
      cipher.load(context, is);

      sealInputs[name] = move(cipher);
    } else if (info.inputType == Type::Plain) {
      seal::Plaintext plain;

      std::string decoded = b64decode(v);
      std::istringstream is(decoded);
      plain.load(context, is);

      sealInputs[name] = move(plain);
    } else {
      // ConstantValue
      throw runtime_error("Not sure how to handle constant value yet");
    }
  }

  return sealInputs;
}

SEALValuation SEALPublic::execute(Program &program,
                                  const SEALValuation &inputs) {
#ifdef EVA_USE_GALOIS
  // Do multicore evaluation if multicore support is available
  GaloisGuard galois;
  MulticoreProgramTraversal programTraverse(program);
#else
  // Otherwise fall back to singlecore evaluation
  ProgramTraversal programTraverse(program);
#endif
  auto sealExecutor = SEALExecutor(program, context, encoder, encryptor,
                                   evaluator, galoisKeys, relinKeys);
  sealExecutor.setInputs(inputs);
  programTraverse.forwardPass(sealExecutor);

  SEALValuation encOutputs(context);
  sealExecutor.getOutputs(encOutputs);
  return encOutputs;
}

std::unordered_map<std::string, std::string> SEALPublic::encodeEncrypted(const SEALValuation &encryptedOutputs) {
  std::unordered_map<std::string, std::string> encoded;
  for (auto &out : encryptedOutputs) {
    auto name = out.first;
    visit(Overloaded{[&](const seal::Ciphertext &cipher) {
                      std::ostringstream buffer;
                      cipher.save(buffer);
                      std::string contents = buffer.str();
                      encoded[name] = b64encode(contents);
                    },
                    [&](const seal::Plaintext &plain) {
                      std::ostringstream buffer;
                      plain.save(buffer);
                      std::string contents = buffer.str();
                      encoded[name] = b64encode(contents);
                    },
                    [&](const std::shared_ptr<ConstantValue> &raw) {
                      // ConstantValue
                      throw runtime_error("Not sure how to handle constant value yet");
                    }},
          out.second);
  }
  return encoded;
}

Valuation SEALSecret::decrypt(const SEALValuation &encOutputs,
                              const CKKSSignature &signature) {
  Valuation outputs;
  std::vector<double> tempVec;
  for (auto &out : encOutputs) {
    auto name = out.first;
    visit(Overloaded{[&](const seal::Ciphertext &cipher) {
                      seal::Plaintext plain;
                      decryptor.decrypt(cipher, plain);
                      encoder.decode(plain, outputs[name]);
                    },
                    [&](const seal::Plaintext &plain) {
                      encoder.decode(plain, outputs[name]);
                    },
                    [&](const std::shared_ptr<ConstantValue> &raw) {
                      auto &scratch = tempVec;
                      outputs[name] = raw->expand(scratch, signature.vecSize);
                    }},
          out.second);
    outputs.at(name).resize(signature.vecSize);
  }
  return outputs;
}

seal::SEALContext getSEALContext(const seal::EncryptionParameters &params) {
  static unordered_map<seal::EncryptionParameters, seal::SEALContext> cache;

  // clean cache except for the required entry
  for (auto iter = cache.begin(); iter != cache.end();) {
    // accessing the context data increases the reference count by one
    // Another reference is incremented by cache entry
    if (iter->second.key_context_data().use_count() == 2 &&
        iter->first != params) {
      iter = cache.erase(iter);
    } else {
      ++iter;
    }
  }

  // find SEALContext
  if (cache.count(params) != 0) {
    seal::SEALContext result = cache.at(params);
    return result;
  } else {
    auto result = cache.emplace(make_pair(
        params, seal::SEALContext(params, true, seal::sec_level_type::none)));
    return result.first->second;
  }
}

tuple<unique_ptr<SEALPublic>, unique_ptr<SEALSecret>>
generateKeys(const CKKSParameters &abstractParams) {
  vector<int> logQs(abstractParams.primeBits.begin(),
                    abstractParams.primeBits.end());

  auto params = seal::EncryptionParameters(seal::scheme_type::ckks);
  params.set_poly_modulus_degree(abstractParams.polyModulusDegree);
  params.set_coeff_modulus(
      seal::CoeffModulus::Create(abstractParams.polyModulusDegree, logQs));

  auto context = getSEALContext(params);

  seal::KeyGenerator keygen(context);
  vector<int> rotationsVec(abstractParams.rotations.begin(),
                           abstractParams.rotations.end());

  seal::PublicKey public_key;
  seal::GaloisKeys galois_keys;
  seal::RelinKeys relin_keys;

  keygen.create_public_key(public_key);
  keygen.create_galois_keys(rotationsVec, galois_keys);
  keygen.create_relin_keys(relin_keys);

  auto secretCtx = make_unique<SEALSecret>(context, keygen.secret_key());
  auto publicCtx =
      make_unique<SEALPublic>(context, public_key, galois_keys, relin_keys);

  return make_tuple(move(publicCtx), move(secretCtx));
}

std::unique_ptr<SEALPublic> recoverKeys(const std::uint32_t &polyModulusDegree,
                                        const std::vector<std::uint32_t> &primeBits,
                                        const std::string &encodedPublicKey,
                                        const std::string &encodedGaloisKey,
                                        const std::string &encodedRelinKey) {
  vector<int> logQs(primeBits.begin(), primeBits.end());

  auto params = seal::EncryptionParameters(seal::scheme_type::ckks);
  params.set_poly_modulus_degree(polyModulusDegree);
  params.set_coeff_modulus(
      seal::CoeffModulus::Create(polyModulusDegree, logQs));

  auto context = getSEALContext(params);

  seal::PublicKey public_key;
  seal::GaloisKeys galois_keys;
  seal::RelinKeys relin_keys;

  std::string decodedPublicKey = b64decode(encodedPublicKey);
  std::istringstream isPublicKey(decodedPublicKey);
  public_key.load(context, isPublicKey);

  std::string decodedGaloisKey = b64decode(encodedGaloisKey);
  std::istringstream isGaloisKey(decodedGaloisKey);
  galois_keys.load(context, isGaloisKey);

  std::string decodedRelinKey = b64decode(encodedRelinKey);
  std::istringstream isRelinKey(decodedRelinKey);
  relin_keys.load(context, isRelinKey);

  auto publicCtx =
      make_unique<SEALPublic>(context, public_key, galois_keys, relin_keys);

  return move(publicCtx);
}

} // namespace eva
