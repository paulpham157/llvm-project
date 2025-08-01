//===- TranslateRegistration.cpp - Register translation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Target/Cpp/CppEmitter.h"
#include "mlir/Tools/mlir-translate/Translation.h"
#include "llvm/Support/CommandLine.h"

using namespace mlir;

namespace mlir {

//===----------------------------------------------------------------------===//
// Cpp registration
//===----------------------------------------------------------------------===//

void registerToCppTranslation() {
  static llvm::cl::opt<bool> declareVariablesAtTop(
      "declare-variables-at-top",
      llvm::cl::desc("Declare variables at top when emitting C/C++"),
      llvm::cl::init(false));

  static llvm::cl::opt<std::string> fileId(
      "file-id", llvm::cl::desc("Emit emitc.file ops with matching id"),
      llvm::cl::init(""));

  TranslateFromMLIRRegistration reg(
      "mlir-to-cpp", "translate from mlir to cpp",
      [](Operation *op, raw_ostream &output) {
        return emitc::translateToCpp(
            op, output,
            /*declareVariablesAtTop=*/declareVariablesAtTop,
            /*fileId=*/fileId);
      },
      [](DialectRegistry &registry) {
        // clang-format off
        registry.insert<cf::ControlFlowDialect,
                        emitc::EmitCDialect,
                        func::FuncDialect>();
        // clang-format on
      });
}

} // namespace mlir
