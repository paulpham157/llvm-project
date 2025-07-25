//===-- ARMSubtarget.h - Define Subtarget for the ARM ----------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the ARM specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_ARM_ARMSUBTARGET_H
#define LLVM_LIB_TARGET_ARM_ARMSUBTARGET_H

#include "ARMBaseInstrInfo.h"
#include "ARMBaseRegisterInfo.h"
#include "ARMConstantPoolValue.h"
#include "ARMFrameLowering.h"
#include "ARMISelLowering.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMSelectionDAGInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/RegisterBankInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/MC/MCSchedule.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include <bitset>
#include <memory>
#include <string>

#define GET_SUBTARGETINFO_HEADER
#include "ARMGenSubtargetInfo.inc"

namespace llvm {

class ARMBaseTargetMachine;
class GlobalValue;
class StringRef;

class ARMSubtarget : public ARMGenSubtargetInfo {
protected:
  enum ARMProcFamilyEnum {
    Others,
#define ARM_PROCESSOR_FAMILY(ENUM) ENUM,
#include "llvm/TargetParser/ARMTargetParserDef.inc"
#undef ARM_PROCESSOR_FAMILY
  };
  enum ARMProcClassEnum {
    None,

    AClass,
    MClass,
    RClass
  };
  enum ARMArchEnum {
#define ARM_ARCHITECTURE(ENUM) ENUM,
#include "llvm/TargetParser/ARMTargetParserDef.inc"
#undef ARM_ARCHITECTURE
  };

public:
  /// What kind of timing do load multiple/store multiple instructions have.
  enum ARMLdStMultipleTiming {
    /// Can load/store 2 registers/cycle.
    DoubleIssue,
    /// Can load/store 2 registers/cycle, but needs an extra cycle if the access
    /// is not 64-bit aligned.
    DoubleIssueCheckUnalignedAccess,
    /// Can load/store 1 register/cycle.
    SingleIssue,
    /// Can load/store 1 register/cycle, but needs an extra cycle for address
    /// computation and potentially also for register writeback.
    SingleIssuePlusExtras,
  };

  /// How the push and pop instructions of callee saved general-purpose
  /// registers should be split.
  enum PushPopSplitVariation {
    /// All GPRs can be pushed in a single instruction.
    /// push {r0-r12, lr}
    /// vpush {d8-d15}
    NoSplit,

    /// R7 and LR must be adjacent, because R7 is the frame pointer, and must
    /// point to a frame record consisting of the previous frame pointer and the
    /// return address.
    /// push {r0-r7, lr}
    /// push {r8-r12}
    /// vpush {d8-d15}
    /// Note that Thumb1 changes this layout when the frame pointer is R11,
    /// using a longer sequence of instructions because R11 can't be used by a
    /// Thumb1 push instruction. This doesn't currently have a separate enum
    /// value, and is handled entriely within Thumb1FrameLowering::emitPrologue.
    SplitR7,

    /// When the stack frame size is not known (because of variable-sized
    /// objects or realignment), Windows SEH requires the callee-saved registers
    /// to be stored in three regions, with R11 and LR below the floating-point
    /// registers.
    /// push {r0-r10, r12}
    /// vpush {d8-d15}
    /// push {r11, lr}
    SplitR11WindowsSEH,

    /// When generating AAPCS-compilant frame chains, R11 is the frame pointer,
    /// and must be pushed adjacent to the return address (LR). Normally this
    /// isn't a problem, because the only register between them is r12, which is
    /// the intra-procedure-call scratch register, so doesn't need to be saved.
    /// However, when PACBTI is in use, r12 contains the authentication code, so
    /// does need to be saved. This means that we need a separate push for R11
    /// and LR.
    /// push {r0-r10, r12}
    /// push {r11, lr}
    /// vpush {d8-d15}
    SplitR11AAPCSSignRA,
  };

protected:
// Bool members corresponding to the SubtargetFeatures defined in tablegen
#define GET_SUBTARGETINFO_MACRO(ATTRIBUTE, DEFAULT, GETTER)                    \
  bool ATTRIBUTE = DEFAULT;
#include "ARMGenSubtargetInfo.inc"

  /// ARMProcFamily - ARM processor family: Cortex-A8, Cortex-A9, and others.
  ARMProcFamilyEnum ARMProcFamily = Others;

  /// ARMProcClass - ARM processor class: None, AClass, RClass or MClass.
  ARMProcClassEnum ARMProcClass = None;

  /// ARMArch - ARM architecture
  ARMArchEnum ARMArch = ARMv4t;

  /// UseMulOps - True if non-microcoded fused integer multiply-add and
  /// multiply-subtract instructions should be used.
  bool UseMulOps = false;

  /// SupportsTailCall - True if the OS supports tail call. The dynamic linker
  /// must be able to synthesize call stubs for interworking between ARM and
  /// Thumb.
  bool SupportsTailCall = false;

  /// RestrictIT - If true, the subtarget disallows generation of complex IT
  ///  blocks.
  bool RestrictIT = false;

  /// stackAlignment - The minimum alignment known to hold of the stack frame on
  /// entry to the function and which must be maintained by every function.
  Align stackAlignment = Align(4);

  /// CPUString - String name of used CPU.
  std::string CPUString;

  unsigned MaxInterleaveFactor = 1;

  /// Clearance before partial register updates (in number of instructions)
  unsigned PartialUpdateClearance = 0;

  /// What kind of timing do load multiple/store multiple have (double issue,
  /// single issue etc).
  ARMLdStMultipleTiming LdStMultipleTiming = SingleIssue;

  /// The adjustment that we need to apply to get the operand latency from the
  /// operand cycle returned by the itinerary data for pre-ISel operands.
  int PreISelOperandLatencyAdjustment = 2;

  /// What alignment is preferred for loop bodies and functions, in log2(bytes).
  unsigned PreferBranchLogAlignment = 0;

  /// The cost factor for MVE instructions, representing the multiple beats an
  // instruction can take. The default is 2, (set in initSubtargetFeatures so
  // that we can use subtarget features less than 2).
  unsigned MVEVectorCostFactor = 0;

  /// OptMinSize - True if we're optimising for minimum code size, equal to
  /// the function attribute.
  bool OptMinSize = false;

  /// IsLittle - The target is Little Endian
  bool IsLittle;

  /// TargetTriple - What processor and OS we're targeting.
  Triple TargetTriple;

  /// SchedModel - Processor specific instruction costs.
  MCSchedModel SchedModel;

  /// Selected instruction itineraries (one entry per itinerary class.)
  InstrItineraryData InstrItins;

  /// Options passed via command line that could influence the target
  const TargetOptions &Options;

  const ARMBaseTargetMachine &TM;

public:
  /// This constructor initializes the data members to match that
  /// of the specified triple.
  ///
  ARMSubtarget(const Triple &TT, const std::string &CPU, const std::string &FS,
               const ARMBaseTargetMachine &TM, bool IsLittle,
               bool MinSize = false);

  /// getMaxInlineSizeThreshold - Returns the maximum memset / memcpy size
  /// that still makes it profitable to inline the call.
  unsigned getMaxInlineSizeThreshold() const {
    return 64;
  }

  /// getMaxMemcpyTPInlineSizeThreshold - Returns the maximum size
  /// that still makes it profitable to inline a llvm.memcpy as a Tail
  /// Predicated loop.
  /// This threshold should only be used for constant size inputs.
  unsigned getMaxMemcpyTPInlineSizeThreshold() const { return 128; }

  /// ParseSubtargetFeatures - Parses features string setting specified
  /// subtarget options.  Definition of function is auto generated by tblgen.
  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  /// initializeSubtargetDependencies - Initializes using a CPU and feature string
  /// so that we can use initializer lists for subtarget initialization.
  ARMSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

  const ARMSelectionDAGInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }

  const ARMBaseInstrInfo *getInstrInfo() const override {
    return InstrInfo.get();
  }

  const ARMTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }

  const ARMFrameLowering *getFrameLowering() const override {
    return FrameLowering.get();
  }

  const ARMBaseRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo->getRegisterInfo();
  }

  const CallLowering *getCallLowering() const override;
  InstructionSelector *getInstructionSelector() const override;
  const LegalizerInfo *getLegalizerInfo() const override;
  const RegisterBankInfo *getRegBankInfo() const override;

private:
  ARMSelectionDAGInfo TSInfo;
  // Either Thumb1FrameLowering or ARMFrameLowering.
  std::unique_ptr<ARMFrameLowering> FrameLowering;
  // Either Thumb1InstrInfo or Thumb2InstrInfo.
  std::unique_ptr<ARMBaseInstrInfo> InstrInfo;
  ARMTargetLowering   TLInfo;

  /// GlobalISel related APIs.
  std::unique_ptr<CallLowering> CallLoweringInfo;
  std::unique_ptr<InstructionSelector> InstSelector;
  std::unique_ptr<LegalizerInfo> Legalizer;
  std::unique_ptr<RegisterBankInfo> RegBankInfo;

  void initSubtargetFeatures(StringRef CPU, StringRef FS);
  ARMFrameLowering *initializeFrameLowering(StringRef CPU, StringRef FS);

  std::bitset<8> CoprocCDE = {};
public:
// Getters for SubtargetFeatures defined in tablegen
#define GET_SUBTARGETINFO_MACRO(ATTRIBUTE, DEFAULT, GETTER)                    \
  bool GETTER() const { return ATTRIBUTE; }
#include "ARMGenSubtargetInfo.inc"

  /// @{
  /// These functions are obsolete, please consider adding subtarget features
  /// or properties instead of calling them.
  bool isCortexA5() const { return ARMProcFamily == CortexA5; }
  bool isCortexA7() const { return ARMProcFamily == CortexA7; }
  bool isCortexA8() const { return ARMProcFamily == CortexA8; }
  bool isCortexA9() const { return ARMProcFamily == CortexA9; }
  bool isCortexA15() const { return ARMProcFamily == CortexA15; }
  bool isSwift()    const { return ARMProcFamily == Swift; }
  bool isCortexM3() const { return ARMProcFamily == CortexM3; }
  bool isCortexM55() const { return ARMProcFamily == CortexM55; }
  bool isCortexM7() const { return ARMProcFamily == CortexM7; }
  bool isCortexM85() const { return ARMProcFamily == CortexM85; }
  bool isLikeA9() const { return isCortexA9() || isCortexA15() || isKrait(); }
  bool isCortexR5() const { return ARMProcFamily == CortexR5; }
  bool isKrait() const { return ARMProcFamily == Krait; }
  /// @}

  bool hasARMOps() const { return !NoARM; }

  bool useNEONForSinglePrecisionFP() const {
    return hasNEON() && hasNEONForFP();
  }

  bool hasVFP2Base() const { return hasVFPv2SP(); }
  bool hasVFP3Base() const { return hasVFPv3D16SP(); }
  bool hasVFP4Base() const { return hasVFPv4D16SP(); }
  bool hasFPARMv8Base() const { return hasFPARMv8D16SP(); }

  bool hasAnyDataBarrier() const {
    return HasDataBarrier || (hasV6Ops() && !isThumb());
  }

  bool useMulOps() const { return UseMulOps; }
  bool useFPVMLx() const { return !SlowFPVMLx; }
  bool useFPVFMx() const {
    return !isTargetDarwin() && hasVFP4Base() && !SlowFPVFMx;
  }
  bool useFPVFMx16() const { return useFPVFMx() && hasFullFP16(); }
  bool useFPVFMx64() const { return useFPVFMx() && hasFP64(); }
  bool hasBaseDSP() const {
    if (isThumb())
      return hasThumb2() && hasDSP();
    else
      return hasV5TEOps();
  }

  /// Return true if the CPU supports any kind of instruction fusion.
  bool hasFusion() const { return hasFuseAES() || hasFuseLiterals(); }

  const Triple &getTargetTriple() const { return TargetTriple; }

  /// @{
  /// These properties are per-module, please use the TargetMachine
  /// TargetTriple.
  bool isTargetDarwin() const { return TargetTriple.isOSDarwin(); }
  bool isTargetIOS() const { return TargetTriple.isiOS(); }
  bool isTargetWatchOS() const { return TargetTriple.isWatchOS(); }
  bool isTargetWatchABI() const { return TargetTriple.isWatchABI(); }
  bool isTargetDriverKit() const { return TargetTriple.isDriverKit(); }
  bool isTargetLinux() const { return TargetTriple.isOSLinux(); }
  bool isTargetNaCl() const { return TargetTriple.isOSNaCl(); }
  bool isTargetNetBSD() const { return TargetTriple.isOSNetBSD(); }
  bool isTargetWindows() const { return TargetTriple.isOSWindows(); }

  bool isTargetCOFF() const { return TargetTriple.isOSBinFormatCOFF(); }
  bool isTargetELF() const { return TargetTriple.isOSBinFormatELF(); }
  bool isTargetMachO() const { return TargetTriple.isOSBinFormatMachO(); }

  bool isTargetAEABI() const { return TargetTriple.isTargetAEABI(); }

  bool isTargetGNUAEABI() const { return TargetTriple.isTargetGNUAEABI(); }

  bool isTargetMuslAEABI() const { return TargetTriple.isTargetMuslAEABI(); }

  // ARM Targets that support EHABI exception handling standard
  // Darwin uses SjLj. Other targets might need more checks.
  bool isTargetEHABICompatible() const {
    return TargetTriple.isTargetEHABICompatible();
  }
  /// @}

  bool isReadTPSoft() const {
    return !(isReadTPTPIDRURW() || isReadTPTPIDRURO() || isReadTPTPIDRPRW());
  }

  bool isTargetAndroid() const { return TargetTriple.isAndroid(); }

  bool isXRaySupported() const override;

  bool isROPI() const;
  bool isRWPI() const;

  bool useMachineScheduler() const { return UseMISched; }
  bool useMachinePipeliner() const { return UseMIPipeliner; }
  bool hasMinSize() const { return OptMinSize; }
  bool isThumb1Only() const { return isThumb() && !hasThumb2(); }
  bool isThumb2() const { return isThumb() && hasThumb2(); }
  bool isMClass() const { return ARMProcClass == MClass; }
  bool isRClass() const { return ARMProcClass == RClass; }
  bool isAClass() const { return ARMProcClass == AClass; }

  bool isR9Reserved() const {
    return isTargetMachO() ? (ReserveR9 || !HasV6Ops) : ReserveR9;
  }

  MCPhysReg getFramePointerReg() const {
    if (isTargetDarwin() ||
        (!isTargetWindows() && isThumb() && !createAAPCSFrameChain()))
      return ARM::R7;
    return ARM::R11;
  }

  enum PushPopSplitVariation
  getPushPopSplitVariation(const MachineFunction &MF) const;

  bool useStride4VFPs() const;

  bool useMovt() const;

  bool supportsTailCall() const { return SupportsTailCall; }

  bool allowsUnalignedMem() const { return !StrictAlign; }

  bool restrictIT() const { return RestrictIT; }

  const std::string & getCPUString() const { return CPUString; }

  bool isLittle() const { return IsLittle; }

  unsigned getMispredictionPenalty() const;

  /// Returns true if machine scheduler should be enabled.
  bool enableMachineScheduler() const override;

  /// Returns true if machine pipeliner should be enabled.
  bool enableMachinePipeliner() const override;
  bool useDFAforSMS() const override;

  /// True for some subtargets at > -O0.
  bool enablePostRAScheduler() const override;

  /// True for some subtargets at > -O0.
  bool enablePostRAMachineScheduler() const override;

  /// Check whether this subtarget wants to use subregister liveness.
  bool enableSubRegLiveness() const override;

  /// Enable use of alias analysis during code generation (during MI
  /// scheduling, DAGCombine, etc.).
  bool useAA() const override { return true; }

  /// getInstrItins - Return the instruction itineraries based on subtarget
  /// selection.
  const InstrItineraryData *getInstrItineraryData() const override {
    return &InstrItins;
  }

  /// getStackAlignment - Returns the minimum alignment known to hold of the
  /// stack frame on entry to the function and which must be maintained by every
  /// function for this subtarget.
  Align getStackAlignment() const { return stackAlignment; }

  // Returns the required alignment for LDRD/STRD instructions
  Align getDualLoadStoreAlignment() const {
    return Align(hasV7Ops() || allowsUnalignedMem() ? 4 : 8);
  }

  unsigned getMaxInterleaveFactor() const { return MaxInterleaveFactor; }

  unsigned getPartialUpdateClearance() const { return PartialUpdateClearance; }

  ARMLdStMultipleTiming getLdStMultipleTiming() const {
    return LdStMultipleTiming;
  }

  int getPreISelOperandLatencyAdjustment() const {
    return PreISelOperandLatencyAdjustment;
  }

  /// True if the GV will be accessed via an indirect symbol.
  bool isGVIndirectSymbol(const GlobalValue *GV) const;

  /// Returns the constant pool modifier needed to access the GV.
  bool isGVInGOT(const GlobalValue *GV) const;

  /// True if fast-isel is used.
  bool useFastISel() const;

  /// Returns the correct return opcode for the current feature set.
  /// Use BX if available to allow mixing thumb/arm code, but fall back
  /// to plain mov pc,lr on ARMv4.
  unsigned getReturnOpcode() const {
    if (isThumb())
      return ARM::tBX_RET;
    if (hasV4TOps())
      return ARM::BX_RET;
    return ARM::MOVPCLR;
  }

  /// Allow movt+movw for PIC global address calculation.
  /// ELF does not have GOT relocations for movt+movw.
  /// ROPI does not use GOT.
  bool allowPositionIndependentMovt() const {
    return isROPI() || !isTargetELF();
  }

  unsigned getPreferBranchLogAlignment() const {
    return PreferBranchLogAlignment;
  }

  unsigned
  getMVEVectorCostFactor(TargetTransformInfo::TargetCostKind CostKind) const {
    if (CostKind == TargetTransformInfo::TCK_CodeSize)
      return 1;
    return MVEVectorCostFactor;
  }

  bool ignoreCSRForAllocationOrder(const MachineFunction &MF,
                                   MCRegister PhysReg) const override;
  unsigned getGPRAllocationOrder(const MachineFunction &MF) const;
};

} // end namespace llvm

#endif  // LLVM_LIB_TARGET_ARM_ARMSUBTARGET_H
