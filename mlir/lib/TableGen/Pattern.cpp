//===- Pattern.cpp - Pattern wrapper class --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pattern wrapper class to simplify using TableGen Record defining a MLIR
// Pattern.
//
//===----------------------------------------------------------------------===//

#include <utility>

#include "mlir/TableGen/Pattern.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"

#define DEBUG_TYPE "mlir-tblgen-pattern"

using namespace mlir;
using namespace tblgen;

using llvm::DagInit;
using llvm::dbgs;
using llvm::DefInit;
using llvm::formatv;
using llvm::IntInit;
using llvm::Record;

//===----------------------------------------------------------------------===//
// DagLeaf
//===----------------------------------------------------------------------===//

bool DagLeaf::isUnspecified() const {
  return isa_and_nonnull<llvm::UnsetInit>(def);
}

bool DagLeaf::isOperandMatcher() const {
  // Operand matchers specify a type constraint.
  return isSubClassOf("TypeConstraint");
}

bool DagLeaf::isAttrMatcher() const {
  // Attribute matchers specify an attribute constraint.
  return isSubClassOf("AttrConstraint");
}

bool DagLeaf::isPropMatcher() const {
  // Property matchers specify a property constraint.
  return isSubClassOf("PropConstraint");
}

bool DagLeaf::isPropDefinition() const {
  // Property matchers specify a property definition.
  return isSubClassOf("Property");
}

bool DagLeaf::isNativeCodeCall() const {
  return isSubClassOf("NativeCodeCall");
}

bool DagLeaf::isConstantAttr() const { return isSubClassOf("ConstantAttr"); }

bool DagLeaf::isEnumCase() const { return isSubClassOf("EnumCase"); }

bool DagLeaf::isConstantProp() const { return isSubClassOf("ConstantProp"); }

bool DagLeaf::isStringAttr() const { return isa<llvm::StringInit>(def); }

Constraint DagLeaf::getAsConstraint() const {
  assert((isOperandMatcher() || isAttrMatcher() || isPropMatcher()) &&
         "the DAG leaf must be operand, attribute, or property");
  return Constraint(cast<DefInit>(def)->getDef());
}

PropConstraint DagLeaf::getAsPropConstraint() const {
  assert(isPropMatcher() && "the DAG leaf must be a property matcher");
  return PropConstraint(cast<DefInit>(def)->getDef());
}

Property DagLeaf::getAsProperty() const {
  assert(isPropDefinition() && "the DAG leaf must be a property definition");
  return Property(cast<DefInit>(def)->getDef());
}

ConstantAttr DagLeaf::getAsConstantAttr() const {
  assert(isConstantAttr() && "the DAG leaf must be constant attribute");
  return ConstantAttr(cast<DefInit>(def));
}

EnumCase DagLeaf::getAsEnumCase() const {
  assert(isEnumCase() && "the DAG leaf must be an enum attribute case");
  return EnumCase(cast<DefInit>(def));
}

ConstantProp DagLeaf::getAsConstantProp() const {
  assert(isConstantProp() && "the DAG leaf must be a constant property value");
  return ConstantProp(cast<DefInit>(def));
}

std::string DagLeaf::getConditionTemplate() const {
  return getAsConstraint().getConditionTemplate();
}

StringRef DagLeaf::getNativeCodeTemplate() const {
  assert(isNativeCodeCall() && "the DAG leaf must be NativeCodeCall");
  return cast<DefInit>(def)->getDef()->getValueAsString("expression");
}

int DagLeaf::getNumReturnsOfNativeCode() const {
  assert(isNativeCodeCall() && "the DAG leaf must be NativeCodeCall");
  return cast<DefInit>(def)->getDef()->getValueAsInt("numReturns");
}

std::string DagLeaf::getStringAttr() const {
  assert(isStringAttr() && "the DAG leaf must be string attribute");
  return def->getAsUnquotedString();
}
bool DagLeaf::isSubClassOf(StringRef superclass) const {
  if (auto *defInit = dyn_cast_or_null<DefInit>(def))
    return defInit->getDef()->isSubClassOf(superclass);
  return false;
}

void DagLeaf::print(raw_ostream &os) const {
  if (def)
    def->print(os);
}

//===----------------------------------------------------------------------===//
// DagNode
//===----------------------------------------------------------------------===//

bool DagNode::isNativeCodeCall() const {
  if (auto *defInit = dyn_cast_or_null<DefInit>(node->getOperator()))
    return defInit->getDef()->isSubClassOf("NativeCodeCall");
  return false;
}

bool DagNode::isOperation() const {
  return !isNativeCodeCall() && !isReplaceWithValue() &&
         !isLocationDirective() && !isReturnTypeDirective() && !isEither() &&
         !isVariadic();
}

StringRef DagNode::getNativeCodeTemplate() const {
  assert(isNativeCodeCall() && "the DAG leaf must be NativeCodeCall");
  return cast<DefInit>(node->getOperator())
      ->getDef()
      ->getValueAsString("expression");
}

int DagNode::getNumReturnsOfNativeCode() const {
  assert(isNativeCodeCall() && "the DAG leaf must be NativeCodeCall");
  return cast<DefInit>(node->getOperator())
      ->getDef()
      ->getValueAsInt("numReturns");
}

StringRef DagNode::getSymbol() const { return node->getNameStr(); }

Operator &DagNode::getDialectOp(RecordOperatorMap *mapper) const {
  const Record *opDef = cast<DefInit>(node->getOperator())->getDef();
  auto [it, inserted] = mapper->try_emplace(opDef);
  if (inserted)
    it->second = std::make_unique<Operator>(opDef);
  return *it->second;
}

int DagNode::getNumOps() const {
  // We want to get number of operations recursively involved in the DAG tree.
  // All other directives should be excluded.
  int count = isOperation() ? 1 : 0;
  for (int i = 0, e = getNumArgs(); i != e; ++i) {
    if (auto child = getArgAsNestedDag(i))
      count += child.getNumOps();
  }
  return count;
}

int DagNode::getNumArgs() const { return node->getNumArgs(); }

bool DagNode::isNestedDagArg(unsigned index) const {
  return isa<DagInit>(node->getArg(index));
}

DagNode DagNode::getArgAsNestedDag(unsigned index) const {
  return DagNode(dyn_cast_or_null<DagInit>(node->getArg(index)));
}

DagLeaf DagNode::getArgAsLeaf(unsigned index) const {
  assert(!isNestedDagArg(index));
  return DagLeaf(node->getArg(index));
}

StringRef DagNode::getArgName(unsigned index) const {
  return node->getArgNameStr(index);
}

bool DagNode::isReplaceWithValue() const {
  auto *dagOpDef = cast<DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "replaceWithValue";
}

bool DagNode::isLocationDirective() const {
  auto *dagOpDef = cast<DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "location";
}

bool DagNode::isReturnTypeDirective() const {
  auto *dagOpDef = cast<DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "returnType";
}

bool DagNode::isEither() const {
  auto *dagOpDef = cast<DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "either";
}

bool DagNode::isVariadic() const {
  auto *dagOpDef = cast<DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "variadic";
}

void DagNode::print(raw_ostream &os) const {
  if (node)
    node->print(os);
}

//===----------------------------------------------------------------------===//
// SymbolInfoMap
//===----------------------------------------------------------------------===//

StringRef SymbolInfoMap::getValuePackName(StringRef symbol, int *index) {
  int idx = -1;
  auto [name, indexStr] = symbol.rsplit("__");

  if (indexStr.consumeInteger(10, idx)) {
    // The second part is not an index; we return the whole symbol as-is.
    return symbol;
  }
  if (index) {
    *index = idx;
  }
  return name;
}

SymbolInfoMap::SymbolInfo::SymbolInfo(
    const Operator *op, SymbolInfo::Kind kind,
    std::optional<DagAndConstant> dagAndConstant)
    : op(op), kind(kind), dagAndConstant(dagAndConstant) {}

int SymbolInfoMap::SymbolInfo::getStaticValueCount() const {
  switch (kind) {
  case Kind::Attr:
  case Kind::Prop:
  case Kind::Operand:
  case Kind::Value:
    return 1;
  case Kind::Result:
    return op->getNumResults();
  case Kind::MultipleValues:
    return getSize();
  }
  llvm_unreachable("unknown kind");
}

std::string SymbolInfoMap::SymbolInfo::getVarName(StringRef name) const {
  return alternativeName ? *alternativeName : name.str();
}

std::string SymbolInfoMap::SymbolInfo::getVarTypeStr(StringRef name) const {
  LLVM_DEBUG(dbgs() << "getVarTypeStr for '" << name << "': ");
  switch (kind) {
  case Kind::Attr: {
    if (op)
      return cast<NamedAttribute *>(op->getArg(getArgIndex()))
          ->attr.getStorageType()
          .str();
    // TODO(suderman): Use a more exact type when available.
    return "::mlir::Attribute";
  }
  case Kind::Prop: {
    if (op)
      return cast<NamedProperty *>(op->getArg(getArgIndex()))
          ->prop.getInterfaceType()
          .str();
    assert(dagAndConstant && dagAndConstant->dag &&
           "generic properties must carry their constraint");
    return reinterpret_cast<const DagLeaf *>(dagAndConstant->dag)
        ->getAsPropConstraint()
        .getInterfaceType()
        .str();
  }
  case Kind::Operand: {
    // Use operand range for captured operands (to support potential variadic
    // operands).
    return "::mlir::Operation::operand_range";
  }
  case Kind::Value: {
    return "::mlir::Value";
  }
  case Kind::MultipleValues: {
    return "::mlir::ValueRange";
  }
  case Kind::Result: {
    // Use the op itself for captured results.
    return op->getQualCppClassName();
  }
  }
  llvm_unreachable("unknown kind");
}

std::string SymbolInfoMap::SymbolInfo::getVarDecl(StringRef name) const {
  LLVM_DEBUG(dbgs() << "getVarDecl for '" << name << "': ");
  std::string varInit = kind == Kind::Operand ? "(op0->getOperands())" : "";
  return std::string(
      formatv("{0} {1}{2};\n", getVarTypeStr(name), getVarName(name), varInit));
}

std::string SymbolInfoMap::SymbolInfo::getArgDecl(StringRef name) const {
  LLVM_DEBUG(dbgs() << "getArgDecl for '" << name << "': ");
  return std::string(
      formatv("{0} &{1}", getVarTypeStr(name), getVarName(name)));
}

std::string SymbolInfoMap::SymbolInfo::getValueAndRangeUse(
    StringRef name, int index, const char *fmt, const char *separator) const {
  LLVM_DEBUG(dbgs() << "getValueAndRangeUse for '" << name << "': ");
  switch (kind) {
  case Kind::Attr: {
    assert(index < 0);
    auto repl = formatv(fmt, name);
    LLVM_DEBUG(dbgs() << repl << " (Attr)\n");
    return std::string(repl);
  }
  case Kind::Prop: {
    assert(index < 0);
    auto repl = formatv(fmt, name);
    LLVM_DEBUG(dbgs() << repl << " (Prop)\n");
    return std::string(repl);
  }
  case Kind::Operand: {
    assert(index < 0);
    auto *operand = cast<NamedTypeConstraint *>(op->getArg(getArgIndex()));
    if (operand->isOptional()) {
      auto repl = formatv(
          fmt, formatv("({0}.empty() ? ::mlir::Value() : *{0}.begin())", name));
      LLVM_DEBUG(dbgs() << repl << " (OptionalOperand)\n");
      return std::string(repl);
    }
    // If this operand is variadic and this SymbolInfo doesn't have a range
    // index, then return the full variadic operand_range. Otherwise, return
    // the value itself.
    if (operand->isVariableLength() && !getVariadicSubIndex().has_value()) {
      auto repl = formatv(fmt, name);
      LLVM_DEBUG(dbgs() << repl << " (VariadicOperand)\n");
      return std::string(repl);
    }
    auto repl = formatv(fmt, formatv("(*{0}.begin())", name));
    LLVM_DEBUG(dbgs() << repl << " (SingleOperand)\n");
    return std::string(repl);
  }
  case Kind::Result: {
    // If `index` is greater than zero, then we are referencing a specific
    // result of a multi-result op. The result can still be variadic.
    if (index >= 0) {
      std::string v =
          std::string(formatv("{0}.getODSResults({1})", name, index));
      if (!op->getResult(index).isVariadic())
        v = std::string(formatv("(*{0}.begin())", v));
      auto repl = formatv(fmt, v);
      LLVM_DEBUG(dbgs() << repl << " (SingleResult)\n");
      return std::string(repl);
    }

    // If this op has no result at all but still we bind a symbol to it, it
    // means we want to capture the op itself.
    if (op->getNumResults() == 0) {
      LLVM_DEBUG(dbgs() << name << " (Op)\n");
      return formatv(fmt, name);
    }

    // We are referencing all results of the multi-result op. A specific result
    // can either be a value or a range. Then join them with `separator`.
    SmallVector<std::string, 4> values;
    values.reserve(op->getNumResults());

    for (int i = 0, e = op->getNumResults(); i < e; ++i) {
      std::string v = std::string(formatv("{0}.getODSResults({1})", name, i));
      if (!op->getResult(i).isVariadic()) {
        v = std::string(formatv("(*{0}.begin())", v));
      }
      values.push_back(std::string(formatv(fmt, v)));
    }
    auto repl = llvm::join(values, separator);
    LLVM_DEBUG(dbgs() << repl << " (VariadicResult)\n");
    return repl;
  }
  case Kind::Value: {
    assert(index < 0);
    assert(op == nullptr);
    auto repl = formatv(fmt, name);
    LLVM_DEBUG(dbgs() << repl << " (Value)\n");
    return std::string(repl);
  }
  case Kind::MultipleValues: {
    assert(op == nullptr);
    assert(index < getSize());
    if (index >= 0) {
      std::string repl =
          formatv(fmt, std::string(formatv("{0}[{1}]", name, index)));
      LLVM_DEBUG(dbgs() << repl << " (MultipleValues)\n");
      return repl;
    }
    // If it doesn't specify certain element, unpack them all.
    auto repl =
        formatv(fmt, std::string(formatv("{0}.begin(), {0}.end()", name)));
    LLVM_DEBUG(dbgs() << repl << " (MultipleValues)\n");
    return std::string(repl);
  }
  }
  llvm_unreachable("unknown kind");
}

std::string SymbolInfoMap::SymbolInfo::getAllRangeUse(
    StringRef name, int index, const char *fmt, const char *separator) const {
  LLVM_DEBUG(dbgs() << "getAllRangeUse for '" << name << "': ");
  switch (kind) {
  case Kind::Attr:
  case Kind::Prop:
  case Kind::Operand: {
    assert(index < 0 && "only allowed for symbol bound to result");
    auto repl = formatv(fmt, name);
    LLVM_DEBUG(dbgs() << repl << " (Operand/Attr/Prop)\n");
    return std::string(repl);
  }
  case Kind::Result: {
    if (index >= 0) {
      auto repl = formatv(fmt, formatv("{0}.getODSResults({1})", name, index));
      LLVM_DEBUG(dbgs() << repl << " (SingleResult)\n");
      return std::string(repl);
    }

    // We are referencing all results of the multi-result op. Each result should
    // have a value range, and then join them with `separator`.
    SmallVector<std::string, 4> values;
    values.reserve(op->getNumResults());

    for (int i = 0, e = op->getNumResults(); i < e; ++i) {
      values.push_back(std::string(
          formatv(fmt, formatv("{0}.getODSResults({1})", name, i))));
    }
    auto repl = llvm::join(values, separator);
    LLVM_DEBUG(dbgs() << repl << " (VariadicResult)\n");
    return repl;
  }
  case Kind::Value: {
    assert(index < 0 && "only allowed for symbol bound to result");
    assert(op == nullptr);
    auto repl = formatv(fmt, formatv("{{{0}}", name));
    LLVM_DEBUG(dbgs() << repl << " (Value)\n");
    return std::string(repl);
  }
  case Kind::MultipleValues: {
    assert(op == nullptr);
    assert(index < getSize());
    if (index >= 0) {
      std::string repl =
          formatv(fmt, std::string(formatv("{0}[{1}]", name, index)));
      LLVM_DEBUG(dbgs() << repl << " (MultipleValues)\n");
      return repl;
    }
    auto repl =
        formatv(fmt, std::string(formatv("{0}.begin(), {0}.end()", name)));
    LLVM_DEBUG(dbgs() << repl << " (MultipleValues)\n");
    return std::string(repl);
  }
  }
  llvm_unreachable("unknown kind");
}

bool SymbolInfoMap::bindOpArgument(DagNode node, StringRef symbol,
                                   const Operator &op, int argIndex,
                                   std::optional<int> variadicSubIndex) {
  StringRef name = getValuePackName(symbol);
  if (name != symbol) {
    auto error = formatv(
        "symbol '{0}' with trailing index cannot bind to op argument", symbol);
    PrintFatalError(loc, error);
  }

  Argument arg = op.getArg(argIndex);
  SymbolInfo symInfo =
      isa<NamedAttribute *>(arg) ? SymbolInfo::getAttr(&op, argIndex)
      : isa<NamedProperty *>(arg)
          ? SymbolInfo::getProp(&op, argIndex)
          : SymbolInfo::getOperand(node, &op, argIndex, variadicSubIndex);

  std::string key = symbol.str();
  if (symbolInfoMap.count(key)) {
    // Only non unique name for the operand is supported.
    if (symInfo.kind != SymbolInfo::Kind::Operand) {
      return false;
    }

    // Cannot add new operand if there is already non operand with the same
    // name.
    if (symbolInfoMap.find(key)->second.kind != SymbolInfo::Kind::Operand) {
      return false;
    }
  }

  symbolInfoMap.emplace(key, symInfo);
  return true;
}

bool SymbolInfoMap::bindOpResult(StringRef symbol, const Operator &op) {
  std::string name = getValuePackName(symbol).str();
  auto inserted = symbolInfoMap.emplace(name, SymbolInfo::getResult(&op));

  return symbolInfoMap.count(inserted->first) == 1;
}

bool SymbolInfoMap::bindValues(StringRef symbol, int numValues) {
  std::string name = getValuePackName(symbol).str();
  if (numValues > 1)
    return bindMultipleValues(name, numValues);
  return bindValue(name);
}

bool SymbolInfoMap::bindValue(StringRef symbol) {
  auto inserted = symbolInfoMap.emplace(symbol.str(), SymbolInfo::getValue());
  return symbolInfoMap.count(inserted->first) == 1;
}

bool SymbolInfoMap::bindMultipleValues(StringRef symbol, int numValues) {
  std::string name = getValuePackName(symbol).str();
  auto inserted =
      symbolInfoMap.emplace(name, SymbolInfo::getMultipleValues(numValues));
  return symbolInfoMap.count(inserted->first) == 1;
}

bool SymbolInfoMap::bindAttr(StringRef symbol) {
  auto inserted = symbolInfoMap.emplace(symbol.str(), SymbolInfo::getAttr());
  return symbolInfoMap.count(inserted->first) == 1;
}

bool SymbolInfoMap::bindProp(StringRef symbol,
                             const PropConstraint &constraint) {
  auto inserted =
      symbolInfoMap.emplace(symbol.str(), SymbolInfo::getProp(&constraint));
  return symbolInfoMap.count(inserted->first) == 1;
}

bool SymbolInfoMap::contains(StringRef symbol) const {
  return find(symbol) != symbolInfoMap.end();
}

SymbolInfoMap::const_iterator SymbolInfoMap::find(StringRef key) const {
  std::string name = getValuePackName(key).str();

  return symbolInfoMap.find(name);
}

SymbolInfoMap::const_iterator
SymbolInfoMap::findBoundSymbol(StringRef key, DagNode node, const Operator &op,
                               int argIndex,
                               std::optional<int> variadicSubIndex) const {
  return findBoundSymbol(
      key, SymbolInfo::getOperand(node, &op, argIndex, variadicSubIndex));
}

SymbolInfoMap::const_iterator
SymbolInfoMap::findBoundSymbol(StringRef key,
                               const SymbolInfo &symbolInfo) const {
  std::string name = getValuePackName(key).str();
  auto range = symbolInfoMap.equal_range(name);

  for (auto it = range.first; it != range.second; ++it)
    if (it->second.dagAndConstant == symbolInfo.dagAndConstant)
      return it;

  return symbolInfoMap.end();
}

std::pair<SymbolInfoMap::iterator, SymbolInfoMap::iterator>
SymbolInfoMap::getRangeOfEqualElements(StringRef key) {
  std::string name = getValuePackName(key).str();

  return symbolInfoMap.equal_range(name);
}

int SymbolInfoMap::count(StringRef key) const {
  std::string name = getValuePackName(key).str();
  return symbolInfoMap.count(name);
}

int SymbolInfoMap::getStaticValueCount(StringRef symbol) const {
  StringRef name = getValuePackName(symbol);
  if (name != symbol) {
    // If there is a trailing index inside symbol, it references just one
    // static value.
    return 1;
  }
  // Otherwise, find how many it represents by querying the symbol's info.
  return find(name)->second.getStaticValueCount();
}

std::string SymbolInfoMap::getValueAndRangeUse(StringRef symbol,
                                               const char *fmt,
                                               const char *separator) const {
  int index = -1;
  StringRef name = getValuePackName(symbol, &index);

  auto it = symbolInfoMap.find(name.str());
  if (it == symbolInfoMap.end()) {
    auto error = formatv("referencing unbound symbol '{0}'", symbol);
    PrintFatalError(loc, error);
  }

  return it->second.getValueAndRangeUse(name, index, fmt, separator);
}

std::string SymbolInfoMap::getAllRangeUse(StringRef symbol, const char *fmt,
                                          const char *separator) const {
  int index = -1;
  StringRef name = getValuePackName(symbol, &index);

  auto it = symbolInfoMap.find(name.str());
  if (it == symbolInfoMap.end()) {
    auto error = formatv("referencing unbound symbol '{0}'", symbol);
    PrintFatalError(loc, error);
  }

  return it->second.getAllRangeUse(name, index, fmt, separator);
}

void SymbolInfoMap::assignUniqueAlternativeNames() {
  llvm::StringSet<> usedNames;

  for (auto symbolInfoIt = symbolInfoMap.begin();
       symbolInfoIt != symbolInfoMap.end();) {
    auto range = symbolInfoMap.equal_range(symbolInfoIt->first);
    auto startRange = range.first;
    auto endRange = range.second;

    auto operandName = symbolInfoIt->first;
    int startSearchIndex = 0;
    for (++startRange; startRange != endRange; ++startRange) {
      // Current operand name is not unique, find a unique one
      // and set the alternative name.
      for (int i = startSearchIndex;; ++i) {
        std::string alternativeName = operandName + std::to_string(i);
        if (!usedNames.contains(alternativeName) &&
            symbolInfoMap.count(alternativeName) == 0) {
          usedNames.insert(alternativeName);
          startRange->second.alternativeName = alternativeName;
          startSearchIndex = i + 1;

          break;
        }
      }
    }

    symbolInfoIt = endRange;
  }
}

//===----------------------------------------------------------------------===//
// Pattern
//==----------------------------------------------------------------------===//

Pattern::Pattern(const Record *def, RecordOperatorMap *mapper)
    : def(*def), recordOpMap(mapper) {}

DagNode Pattern::getSourcePattern() const {
  return DagNode(def.getValueAsDag("sourcePattern"));
}

int Pattern::getNumResultPatterns() const {
  auto *results = def.getValueAsListInit("resultPatterns");
  return results->size();
}

DagNode Pattern::getResultPattern(unsigned index) const {
  auto *results = def.getValueAsListInit("resultPatterns");
  return DagNode(cast<DagInit>(results->getElement(index)));
}

void Pattern::collectSourcePatternBoundSymbols(SymbolInfoMap &infoMap) {
  LLVM_DEBUG(dbgs() << "start collecting source pattern bound symbols\n");
  collectBoundSymbols(getSourcePattern(), infoMap, /*isSrcPattern=*/true);
  LLVM_DEBUG(dbgs() << "done collecting source pattern bound symbols\n");

  LLVM_DEBUG(dbgs() << "start assigning alternative names for symbols\n");
  infoMap.assignUniqueAlternativeNames();
  LLVM_DEBUG(dbgs() << "done assigning alternative names for symbols\n");
}

void Pattern::collectResultPatternBoundSymbols(SymbolInfoMap &infoMap) {
  LLVM_DEBUG(dbgs() << "start collecting result pattern bound symbols\n");
  for (int i = 0, e = getNumResultPatterns(); i < e; ++i) {
    auto pattern = getResultPattern(i);
    collectBoundSymbols(pattern, infoMap, /*isSrcPattern=*/false);
  }
  LLVM_DEBUG(dbgs() << "done collecting result pattern bound symbols\n");
}

const Operator &Pattern::getSourceRootOp() {
  return getSourcePattern().getDialectOp(recordOpMap);
}

Operator &Pattern::getDialectOp(DagNode node) {
  return node.getDialectOp(recordOpMap);
}

std::vector<AppliedConstraint> Pattern::getConstraints() const {
  auto *listInit = def.getValueAsListInit("constraints");
  std::vector<AppliedConstraint> ret;
  ret.reserve(listInit->size());

  for (auto *it : *listInit) {
    auto *dagInit = dyn_cast<DagInit>(it);
    if (!dagInit)
      PrintFatalError(&def, "all elements in Pattern multi-entity "
                            "constraints should be DAG nodes");

    std::vector<std::string> entities;
    entities.reserve(dagInit->arg_size());
    for (auto *argName : dagInit->getArgNames()) {
      if (!argName) {
        PrintFatalError(
            &def,
            "operands to additional constraints can only be symbol references");
      }
      entities.emplace_back(argName->getValue());
    }

    ret.emplace_back(cast<DefInit>(dagInit->getOperator())->getDef(),
                     dagInit->getNameStr(), std::move(entities));
  }
  return ret;
}

int Pattern::getNumSupplementalPatterns() const {
  auto *results = def.getValueAsListInit("supplementalPatterns");
  return results->size();
}

DagNode Pattern::getSupplementalPattern(unsigned index) const {
  auto *results = def.getValueAsListInit("supplementalPatterns");
  return DagNode(cast<DagInit>(results->getElement(index)));
}

int Pattern::getBenefit() const {
  // The initial benefit value is a heuristic with number of ops in the source
  // pattern.
  int initBenefit = getSourcePattern().getNumOps();
  const DagInit *delta = def.getValueAsDag("benefitDelta");
  if (delta->getNumArgs() != 1 || !isa<IntInit>(delta->getArg(0))) {
    PrintFatalError(&def,
                    "The 'addBenefit' takes and only takes one integer value");
  }
  return initBenefit + dyn_cast<IntInit>(delta->getArg(0))->getValue();
}

std::vector<Pattern::IdentifierLine> Pattern::getLocation() const {
  std::vector<std::pair<StringRef, unsigned>> result;
  result.reserve(def.getLoc().size());
  for (auto loc : def.getLoc()) {
    unsigned buf = llvm::SrcMgr.FindBufferContainingLoc(loc);
    assert(buf && "invalid source location");
    result.emplace_back(
        llvm::SrcMgr.getBufferInfo(buf).Buffer->getBufferIdentifier(),
        llvm::SrcMgr.getLineAndColumn(loc, buf).first);
  }
  return result;
}

void Pattern::verifyBind(bool result, StringRef symbolName) {
  if (!result) {
    auto err = formatv("symbol '{0}' bound more than once", symbolName);
    PrintFatalError(&def, err);
  }
}

void Pattern::collectBoundSymbols(DagNode tree, SymbolInfoMap &infoMap,
                                  bool isSrcPattern) {
  auto treeName = tree.getSymbol();
  auto numTreeArgs = tree.getNumArgs();

  if (tree.isNativeCodeCall()) {
    if (!treeName.empty()) {
      if (!isSrcPattern) {
        LLVM_DEBUG(dbgs() << "found symbol bound to NativeCodeCall: "
                          << treeName << '\n');
        verifyBind(
            infoMap.bindValues(treeName, tree.getNumReturnsOfNativeCode()),
            treeName);
      } else {
        PrintFatalError(&def,
                        formatv("binding symbol '{0}' to NativecodeCall in "
                                "MatchPattern is not supported",
                                treeName));
      }
    }

    for (int i = 0; i != numTreeArgs; ++i) {
      if (auto treeArg = tree.getArgAsNestedDag(i)) {
        // This DAG node argument is a DAG node itself. Go inside recursively.
        collectBoundSymbols(treeArg, infoMap, isSrcPattern);
        continue;
      }

      if (!isSrcPattern)
        continue;

      // We can only bind symbols to arguments in source pattern. Those
      // symbols are referenced in result patterns.
      auto treeArgName = tree.getArgName(i);

      // `$_` is a special symbol meaning ignore the current argument.
      if (!treeArgName.empty() && treeArgName != "_") {
        DagLeaf leaf = tree.getArgAsLeaf(i);

        // In (NativeCodeCall<"Foo($_self, $0, $1, $2, $3)"> I8Attr:$a, I8:$b,
        //     $c, I8Prop:$d),
        if (leaf.isUnspecified()) {
          // This is case of $c, a Value without any constraints.
          verifyBind(infoMap.bindValue(treeArgName), treeArgName);
        } else if (leaf.isPropMatcher()) {
          // This is case of $d, a binding to a certain property.
          auto propConstraint = leaf.getAsPropConstraint();
          if (propConstraint.getInterfaceType().empty()) {
            PrintFatalError(&def,
                            formatv("binding symbol '{0}' in NativeCodeCall to "
                                    "a property constraint without specifying "
                                    "that constraint's type is unsupported",
                                    treeArgName));
          }
          verifyBind(infoMap.bindProp(treeArgName, propConstraint),
                     treeArgName);
        } else {
          auto constraint = leaf.getAsConstraint();
          bool isAttr = leaf.isAttrMatcher() || leaf.isEnumCase() ||
                        leaf.isConstantAttr() ||
                        constraint.getKind() == Constraint::Kind::CK_Attr;

          if (isAttr) {
            // This is case of $a, a binding to a certain attribute.
            verifyBind(infoMap.bindAttr(treeArgName), treeArgName);
            continue;
          }

          // This is case of $b, a binding to a certain type.
          verifyBind(infoMap.bindValue(treeArgName), treeArgName);
        }
      }
    }

    return;
  }

  if (tree.isOperation()) {
    auto &op = getDialectOp(tree);
    auto numOpArgs = op.getNumArgs();
    int numEither = 0;

    // We need to exclude the trailing directives and `either` directive groups
    // two operands of the operation.
    int numDirectives = 0;
    for (int i = numTreeArgs - 1; i >= 0; --i) {
      if (auto dagArg = tree.getArgAsNestedDag(i)) {
        if (dagArg.isLocationDirective() || dagArg.isReturnTypeDirective())
          ++numDirectives;
        else if (dagArg.isEither())
          ++numEither;
      }
    }

    if (numOpArgs != numTreeArgs - numDirectives + numEither) {
      auto err =
          formatv("op '{0}' argument number mismatch: "
                  "{1} in pattern vs. {2} in definition",
                  op.getOperationName(), numTreeArgs + numEither, numOpArgs);
      PrintFatalError(&def, err);
    }

    // The name attached to the DAG node's operator is for representing the
    // results generated from this op. It should be remembered as bound results.
    if (!treeName.empty()) {
      LLVM_DEBUG(dbgs() << "found symbol bound to op result: " << treeName
                        << '\n');
      verifyBind(infoMap.bindOpResult(treeName, op), treeName);
    }

    // The operand in `either` DAG should be bound to the operation in the
    // parent DagNode.
    auto collectSymbolInEither = [&](DagNode parent, DagNode tree,
                                     int opArgIdx) {
      for (int i = 0; i < tree.getNumArgs(); ++i, ++opArgIdx) {
        if (DagNode subTree = tree.getArgAsNestedDag(i)) {
          collectBoundSymbols(subTree, infoMap, isSrcPattern);
        } else {
          auto argName = tree.getArgName(i);
          if (!argName.empty() && argName != "_") {
            verifyBind(infoMap.bindOpArgument(parent, argName, op, opArgIdx),
                       argName);
          }
        }
      }
    };

    // The operand in `variadic` DAG should be bound to the operation in the
    // parent DagNode. The range index must be included as well to distinguish
    // (potentially) repeating argName within the `variadic` DAG.
    auto collectSymbolInVariadic = [&](DagNode parent, DagNode tree,
                                       int opArgIdx) {
      auto treeName = tree.getSymbol();
      if (!treeName.empty()) {
        // If treeName is specified, bind to the full variadic operand_range.
        verifyBind(infoMap.bindOpArgument(parent, treeName, op, opArgIdx,
                                          std::nullopt),
                   treeName);
      }

      for (int i = 0; i < tree.getNumArgs(); ++i) {
        if (DagNode subTree = tree.getArgAsNestedDag(i)) {
          collectBoundSymbols(subTree, infoMap, isSrcPattern);
        } else {
          auto argName = tree.getArgName(i);
          if (!argName.empty() && argName != "_") {
            verifyBind(infoMap.bindOpArgument(parent, argName, op, opArgIdx,
                                              /*variadicSubIndex=*/i),
                       argName);
          }
        }
      }
    };

    for (int i = 0, opArgIdx = 0; i != numTreeArgs; ++i, ++opArgIdx) {
      if (auto treeArg = tree.getArgAsNestedDag(i)) {
        if (treeArg.isEither()) {
          collectSymbolInEither(tree, treeArg, opArgIdx);
          // `either` DAG is *flattened*. For example,
          //
          //  (FooOp (either arg0, arg1), arg2)
          //
          //  can be viewed as:
          //
          //  (FooOp arg0, arg1, arg2)
          ++opArgIdx;
        } else if (treeArg.isVariadic()) {
          collectSymbolInVariadic(tree, treeArg, opArgIdx);
        } else {
          // This DAG node argument is a DAG node itself. Go inside recursively.
          collectBoundSymbols(treeArg, infoMap, isSrcPattern);
        }
        continue;
      }

      if (isSrcPattern) {
        // We can only bind symbols to op arguments in source pattern. Those
        // symbols are referenced in result patterns.
        auto treeArgName = tree.getArgName(i);
        // `$_` is a special symbol meaning ignore the current argument.
        if (!treeArgName.empty() && treeArgName != "_") {
          LLVM_DEBUG(dbgs() << "found symbol bound to op argument: "
                            << treeArgName << '\n');
          verifyBind(infoMap.bindOpArgument(tree, treeArgName, op, opArgIdx),
                     treeArgName);
        }
      }
    }
    return;
  }

  if (!treeName.empty()) {
    PrintFatalError(
        &def, formatv("binding symbol '{0}' to non-operation/native code call "
                      "unsupported right now",
                      treeName));
  }
}
