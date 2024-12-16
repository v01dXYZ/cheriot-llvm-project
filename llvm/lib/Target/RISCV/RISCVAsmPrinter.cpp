//===-- RISCVAsmPrinter.cpp - RISC-V LLVM assembly writer -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the RISC-V assembly language.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "MCTargetDesc/RISCVInstPrinter.h"
#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVTargetStreamer.h"
#include "RISCV.h"
#include "RISCVMachineFunctionInfo.h"
#include "RISCVTargetMachine.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation/HWAddressSanitizer.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

STATISTIC(RISCVNumInstrsCompressed,
          "Number of RISC-V Compressed instructions emitted");

namespace {
class RISCVAsmPrinter : public AsmPrinter {
  const RISCVSubtarget *STI;

public:
  explicit RISCVAsmPrinter(TargetMachine &TM,
                           std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)) {}

  StringRef getPassName() const override { return "RISC-V Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void emitInstruction(const MachineInstr *MI) override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;

  void EmitToStreamer(MCStreamer &S, const MCInst &Inst);
  bool emitPseudoExpansionLowering(MCStreamer &OutStreamer,
                                   const MachineInstr *MI);

  typedef std::tuple<unsigned, uint32_t> HwasanMemaccessTuple;
  std::map<HwasanMemaccessTuple, MCSymbol *> HwasanMemaccessSymbols;
  void LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI);
  void LowerKCFI_CHECK(const MachineInstr &MI);
  void EmitHwasanMemaccessSymbols(Module &M);

  // Wrapper needed for tblgenned pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const;

  void emitStartOfAsmFile(Module &M) override;
  void emitEndOfAsmFile(Module &M) override;

  void emitFunctionEntryLabel() override;

private:
  /**
   * Struct describing compartment exports that must be emitted for this
   * compilation unit.
   */
  struct CompartmentExport
  {
    /// The compartment name for the function.
    std::string CompartmentName;
    /// The IR function corresponding to the function.
    const Function &Fn;
    /// The symbol for the function
    MCSymbol *FnSym;
    /// The number of registers that are live on entry to this function
    int LiveIns;
    /// Emit this export as a local symbol even if the function is not local.
    bool forceLocal = false;
    /// The size in bytes of the stack frame, 0 if not used.
    uint32_t stackSize = 0;
  };
  SmallVector<CompartmentExport, 1> CompartmentEntries;
  void emitAttributes();

  void emitNTLHint(const MachineInstr *MI);

  bool lowerToMCInst(const MachineInstr *MI, MCInst &OutMI);
};
}

void RISCVAsmPrinter::EmitToStreamer(MCStreamer &S, const MCInst &Inst) {
  MCInst CInst;
  bool Res = RISCVRVC::compress(CInst, Inst, *STI);
  if (Res)
    ++RISCVNumInstrsCompressed;
  AsmPrinter::EmitToStreamer(*OutStreamer, Res ? CInst : Inst);
}

// Simple pseudo-instructions have their lowering (with expansion to real
// instructions) auto-generated.
#include "RISCVGenMCPseudoLowering.inc"

// If the target supports Zihintntl and the instruction has a nontemporal
// MachineMemOperand, emit an NTLH hint instruction before it.
void RISCVAsmPrinter::emitNTLHint(const MachineInstr *MI) {
  if (!STI->hasStdExtZihintntl())
    return;

  if (MI->memoperands_empty())
    return;

  MachineMemOperand *MMO = *(MI->memoperands_begin());
  if (!MMO->isNonTemporal())
    return;

  unsigned NontemporalMode = 0;
  if (MMO->getFlags() & MONontemporalBit0)
    NontemporalMode += 0b1;
  if (MMO->getFlags() & MONontemporalBit1)
    NontemporalMode += 0b10;

  MCInst Hint;
  if (STI->hasStdExtCOrZca() && STI->enableRVCHintInstrs())
    Hint.setOpcode(RISCV::C_ADD_HINT);
  else
    Hint.setOpcode(RISCV::ADD);

  Hint.addOperand(MCOperand::createReg(RISCV::X0));
  Hint.addOperand(MCOperand::createReg(RISCV::X0));
  Hint.addOperand(MCOperand::createReg(RISCV::X2 + NontemporalMode));

  EmitToStreamer(*OutStreamer, Hint);
}

void RISCVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  RISCV_MC::verifyInstructionPredicates(MI->getOpcode(),
                                        getSubtargetInfo().getFeatureBits());

  emitNTLHint(MI);

  // Do any auto-generated pseudo lowerings.
  if (emitPseudoExpansionLowering(*OutStreamer, MI))
    return;


  switch (MI->getOpcode()) {
  case RISCV::HWASAN_CHECK_MEMACCESS_SHORTGRANULES:
    LowerHWASAN_CHECK_MEMACCESS(*MI);
    return;
  case RISCV::KCFI_CHECK:
    LowerKCFI_CHECK(*MI);
    return;
  case RISCV::PseudoRVVInitUndefM1:
  case RISCV::PseudoRVVInitUndefM2:
  case RISCV::PseudoRVVInitUndefM4:
  case RISCV::PseudoRVVInitUndefM8:
    return;
  }

  MCInst OutInst;
  if (!lowerToMCInst(MI, OutInst))
    EmitToStreamer(*OutStreamer, OutInst);
}

bool RISCVAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                      const char *ExtraCode, raw_ostream &OS) {
  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS))
    return false;

  const MachineOperand &MO = MI->getOperand(OpNo);
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    case 'z':      // Print zero register if zero, regular printing otherwise.
      if (MO.isImm() && MO.getImm() == 0) {
        OS << RISCVInstPrinter::getRegisterName(RISCV::X0);
        return false;
      }
      break;
    case 'i': // Literal 'i' if operand is not a register.
      if (!MO.isReg())
        OS << 'i';
      return false;
    }
  }

  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return false;
  case MachineOperand::MO_Register:
    OS << RISCVInstPrinter::getRegisterName(MO.getReg());
    return false;
  case MachineOperand::MO_GlobalAddress:
    PrintSymbolOperand(MO, OS);
    return false;
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(OS, MAI);
    return false;
  }
  default:
    break;
  }

  return true;
}

bool RISCVAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                            unsigned OpNo,
                                            const char *ExtraCode,
                                            raw_ostream &OS) {
  if (ExtraCode)
    return AsmPrinter::PrintAsmMemoryOperand(MI, OpNo, ExtraCode, OS);

  const MachineOperand &AddrReg = MI->getOperand(OpNo);
  assert(MI->getNumOperands() > OpNo + 1 && "Expected additional operand");
  const MachineOperand &DispImm = MI->getOperand(OpNo + 1);
  // All memory operands should have a register and an immediate operand (see
  // RISCVDAGToDAGISel::SelectInlineAsmMemoryOperand).
  if (!AddrReg.isReg())
    return true;
  if (!DispImm.isImm())
    return true;

  OS << DispImm.getImm() << "("
     << RISCVInstPrinter::getRegisterName(AddrReg.getReg()) << ")";
  return false;
}

bool RISCVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<RISCVSubtarget>();

  SetupMachineFunction(MF);
  emitFunctionBody();
  auto &Fn = MF.getFunction();
  // The low 3 bits of the flags field specify the number of registers to
  // clear.  The next two provide the set of
  int interruptFlag = 0;
  if (Fn.hasFnAttribute("interrupt-state"))
    interruptFlag = StringSwitch<int>(
                        Fn.getFnAttribute("interrupt-state").getValueAsString())
                        .Case("enabled", 1 << 3)
                        .Case("disabled", 2 << 3)
                        .Default(0);

  // For the CHERI MCU ABI, find the highest used argument register.  The
  // switcher will zero all of the ones above this.
  auto countUsedArgRegisters = [](auto const &MF) -> int {
    static constexpr int ArgRegCount = 7;
    static const MCPhysReg ArgGPCRsE[ArgRegCount] = {
        RISCV::C10, RISCV::C11, RISCV::C12, RISCV::C13,
        RISCV::C14, RISCV::C15, RISCV::C5};
    auto LiveIns = MF.getRegInfo().liveins();
    auto *TRI = MF.getRegInfo().getTargetRegisterInfo();
    int NumArgRegs = 0;
    for (auto LI : LiveIns)
      for (int i = 0; i < ArgRegCount; i++)
        if ((ArgGPCRsE[i] == LI.first) ||
            TRI->isSubRegister(ArgGPCRsE[i], LI.first)) {
          NumArgRegs = std::max(NumArgRegs, i + 1);
          break;
        }
    return NumArgRegs;
  };

  if (Fn.getCallingConv() == CallingConv::CHERI_CCallee) {
    Function &Fn = MF.getFunction();
    uint32_t stackSize;
    if (Fn.hasFnAttribute("minimum-stack-size")) {
      bool converted =
          to_integer(Fn.getFnAttribute("minimum-stack-size").getValueAsString(),
                     stackSize);
      assert(converted && "minimum-stack-size attribute must be an integer");
      (void)converted;
    } else
      stackSize = MF.getFrameInfo().getStackSize();
    // FIXME: Get stack size as function attribute if specified
    CompartmentEntries.push_back(
        {std::string(Fn.getFnAttribute("cheri-compartment").getValueAsString()),
         Fn, OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag, false, stackSize});
  } else if (Fn.getCallingConv() == CallingConv::CHERI_LibCall)
    CompartmentEntries.push_back(
        {"libcalls", Fn,
         OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag});
  else if (interruptFlag != 0)
    CompartmentEntries.push_back(
        {std::string(Fn.getFnAttribute("cheri-compartment").getValueAsString()),
         Fn, OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag, true});

  return false;
}

void RISCVAsmPrinter::emitStartOfAsmFile(Module &M) {
  RISCVTargetStreamer &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());
  if (const MDString *ModuleTargetABI =
          dyn_cast_or_null<MDString>(M.getModuleFlag("target-abi")))
    RTS.setTargetABI(RISCVABI::getTargetABI(ModuleTargetABI->getString(), TM.getTargetTriple()));
  if (TM.getTargetTriple().isOSBinFormatELF())
    emitAttributes();
}

void RISCVAsmPrinter::emitEndOfAsmFile(Module &M) {
  RISCVTargetStreamer &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());

  if (!CompartmentEntries.empty()) {
    auto &C = OutStreamer->getContext();
    auto *Exports = C.getELFSection(".compartment_exports", ELF::SHT_PROGBITS,
                                    ELF::SHF_ALLOC | ELF::SHF_GNU_RETAIN);
    OutStreamer->switchSection(Exports);
    auto CompartmentStartSym = C.getOrCreateSymbol("__compartment_pcc_start");
    for (auto &Entry : CompartmentEntries) {
      std::string ExportName = getImportExportTableName(
          Entry.CompartmentName, Entry.Fn.getName(), Entry.Fn.getCallingConv(),
          /*IsImport*/ false);
      auto Sym = C.getOrCreateSymbol(ExportName);
      OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeObject);
      // If the function isn't global, don't make its export table entry global
      // either.  Two different compilation units in the same compartment may
      // export different static things.
      if (Entry.Fn.hasExternalLinkage() && !Entry.forceLocal)
        OutStreamer->emitSymbolAttribute(Sym, MCSA_Global);
      OutStreamer->emitValueToAlignment(Align(4));
      OutStreamer->emitLabel(Sym);
      emitLabelDifference(Entry.FnSym, CompartmentStartSym, 2);
      auto stackSize = Entry.stackSize;
      // Round up to multiple of 8 and divide by 8.
      stackSize = (stackSize + 7) / 8;
      // TODO: We should probably warn if the std::min truncates here.
      OutStreamer->emitIntValue(std::min(uint32_t(255), stackSize), 1);
      OutStreamer->emitIntValue(Entry.LiveIns, 1);
      OutStreamer->emitELFSize(Sym, MCConstantExpr::create(4, C));
    }
  }
  // Generate CHERIoT imports if there are any.
  auto &CHERIoTCompartmentImports =
      static_cast<RISCVTargetMachine &>(TM).ImportedFunctions;
  if (!CHERIoTCompartmentImports.empty()) {
    auto &C = OutStreamer->getContext();

    for (auto &Entry : CHERIoTCompartmentImports) {
      // Import entries are capability-sized entries.  The second word is
      // zero, the first is the address of the corresponding export table
      // entry.

      // Public symbols must be COMDATs so that they can be merged across
      // compilation units.  Private ones must not be.
      auto *Section =
          Entry.IsPublic
              ? C.getELFSection(".compartment_imports", ELF::SHT_PROGBITS,
                                ELF::SHF_ALLOC | ELF::SHF_GROUP, 0,
                                Entry.ImportName, true)
              : C.getELFSection(".compartment_imports", ELF::SHT_PROGBITS,
                                ELF::SHF_ALLOC);
      OutStreamer->switchSection(Section);
      auto Sym = C.getOrCreateSymbol(Entry.ImportName);
      auto ExportSym = C.getOrCreateSymbol(Entry.ExportName);
      OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeObject);
      if (Entry.IsPublic)
        OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
      OutStreamer->emitValueToAlignment(Align(8));
      OutStreamer->emitLabel(Sym);
      // Library imports have their low bit set.
      if (Entry.IsLibrary)
        OutStreamer->emitValue(
            MCBinaryExpr::createAdd(MCSymbolRefExpr::create(ExportSym, C),
                                    MCConstantExpr::create(1, C), C),
            4);
      else
        OutStreamer->emitValue(MCSymbolRefExpr::create(ExportSym, C), 4);
      OutStreamer->emitIntValue(0, 4);
      OutStreamer->emitELFSize(Sym, MCConstantExpr::create(8, C));
    }
  }

  if (TM.getTargetTriple().isOSBinFormatELF())
    RTS.finishAttributeSection();
  EmitHwasanMemaccessSymbols(M);
}

void RISCVAsmPrinter::emitAttributes() {
  RISCVTargetStreamer &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());
  // Use MCSubtargetInfo from TargetMachine. Individual functions may have
  // attributes that differ from other functions in the module and we have no
  // way to know which function is correct.
  RTS.emitTargetAttributes(*TM.getMCSubtargetInfo(), /*EmitStackAlign*/ true);
}

void RISCVAsmPrinter::emitFunctionEntryLabel() {
  const auto *RMFI = MF->getInfo<RISCVMachineFunctionInfo>();
  if (RMFI->isVectorCall()) {
    auto &RTS =
        static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());
    RTS.emitDirectiveVariantCC(*CurrentFnSym);
  }
  AsmPrinter::emitFunctionEntryLabel();
  auto &Subtarget = MF->getSubtarget<RISCVSubtarget>();
  const MachineJumpTableInfo *MJTI = MF->getJumpTableInfo();
  if (RISCVABI::isCheriPureCapABI(Subtarget.getTargetABI()) &&
      MJTI && !MJTI->isEmpty()) {
    MCSymbol *Sym = getSymbolWithGlobalValueBase(&MF->getFunction(),
                                                 "$jump_table_base");
    OutStreamer->emitLabel(Sym);
  }
}

// Force static initialization.
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeRISCVAsmPrinter() {
  RegisterAsmPrinter<RISCVAsmPrinter> X(getTheRISCV32Target());
  RegisterAsmPrinter<RISCVAsmPrinter> Y(getTheRISCV64Target());
}

void RISCVAsmPrinter::LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI) {
  Register Reg = MI.getOperand(0).getReg();
  uint32_t AccessInfo = MI.getOperand(1).getImm();
  MCSymbol *&Sym =
      HwasanMemaccessSymbols[HwasanMemaccessTuple(Reg, AccessInfo)];
  if (!Sym) {
    // FIXME: Make this work on non-ELF.
    if (!TM.getTargetTriple().isOSBinFormatELF())
      report_fatal_error("llvm.hwasan.check.memaccess only supported on ELF");

    std::string SymName = "__hwasan_check_x" + utostr(Reg - RISCV::X0) + "_" +
                          utostr(AccessInfo) + "_short";
    Sym = OutContext.getOrCreateSymbol(SymName);
  }
  auto Res = MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_None, OutContext);
  auto Expr = RISCVMCExpr::create(Res, RISCVMCExpr::VK_RISCV_CALL, OutContext);

  EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::PseudoCALL).addExpr(Expr));
}

void RISCVAsmPrinter::LowerKCFI_CHECK(const MachineInstr &MI) {
  Register AddrReg = MI.getOperand(0).getReg();
  assert(std::next(MI.getIterator())->isCall() &&
         "KCFI_CHECK not followed by a call instruction");
  assert(std::next(MI.getIterator())->getOperand(0).getReg() == AddrReg &&
         "KCFI_CHECK call target doesn't match call operand");

  // Temporary registers for comparing the hashes. If a register is used
  // for the call target, or reserved by the user, we can clobber another
  // temporary register as the check is immediately followed by the
  // call. The check defaults to X6/X7, but can fall back to X28-X31 if
  // needed.
  unsigned ScratchRegs[] = {RISCV::X6, RISCV::X7};
  unsigned NextReg = RISCV::X28;
  auto isRegAvailable = [&](unsigned Reg) {
    return Reg != AddrReg && !STI->isRegisterReservedByUser(Reg);
  };
  for (auto &Reg : ScratchRegs) {
    if (isRegAvailable(Reg))
      continue;
    while (!isRegAvailable(NextReg))
      ++NextReg;
    Reg = NextReg++;
    if (Reg > RISCV::X31)
      report_fatal_error("Unable to find scratch registers for KCFI_CHECK");
  }

  if (AddrReg == RISCV::X0) {
    // Checking X0 makes no sense. Instead of emitting a load, zero
    // ScratchRegs[0].
    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::ADDI)
                                     .addReg(ScratchRegs[0])
                                     .addReg(RISCV::X0)
                                     .addImm(0));
  } else {
    // Adjust the offset for patchable-function-prefix. This assumes that
    // patchable-function-prefix is the same for all functions.
    int NopSize = STI->hasStdExtCOrZca() ? 2 : 4;
    int64_t PrefixNops = 0;
    (void)MI.getMF()
        ->getFunction()
        .getFnAttribute("patchable-function-prefix")
        .getValueAsString()
        .getAsInteger(10, PrefixNops);

    // Load the target function type hash.
    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::LW)
                                     .addReg(ScratchRegs[0])
                                     .addReg(AddrReg)
                                     .addImm(-(PrefixNops * NopSize + 4)));
  }

  // Load the expected 32-bit type hash.
  const int64_t Type = MI.getOperand(1).getImm();
  const int64_t Hi20 = ((Type + 0x800) >> 12) & 0xFFFFF;
  const int64_t Lo12 = SignExtend64<12>(Type);
  if (Hi20) {
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::LUI).addReg(ScratchRegs[1]).addImm(Hi20));
  }
  if (Lo12 || Hi20 == 0) {
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder((STI->hasFeature(RISCV::Feature64Bit) && Hi20)
                                     ? RISCV::ADDIW
                                     : RISCV::ADDI)
                       .addReg(ScratchRegs[1])
                       .addReg(ScratchRegs[1])
                       .addImm(Lo12));
  }

  // Compare the hashes and trap if there's a mismatch.
  MCSymbol *Pass = OutContext.createTempSymbol();
  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(RISCV::BEQ)
                     .addReg(ScratchRegs[0])
                     .addReg(ScratchRegs[1])
                     .addExpr(MCSymbolRefExpr::create(Pass, OutContext)));

  MCSymbol *Trap = OutContext.createTempSymbol();
  OutStreamer->emitLabel(Trap);
  EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::EBREAK));
  emitKCFITrapEntry(*MI.getMF(), Trap);
  OutStreamer->emitLabel(Pass);
}

void RISCVAsmPrinter::EmitHwasanMemaccessSymbols(Module &M) {
  if (HwasanMemaccessSymbols.empty())
    return;

  assert(TM.getTargetTriple().isOSBinFormatELF());
  // Use MCSubtargetInfo from TargetMachine. Individual functions may have
  // attributes that differ from other functions in the module and we have no
  // way to know which function is correct.
  const MCSubtargetInfo &MCSTI = *TM.getMCSubtargetInfo();

  MCSymbol *HwasanTagMismatchV2Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch_v2");
  // Annotate symbol as one having incompatible calling convention, so
  // run-time linkers can instead eagerly bind this function.
  auto &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());
  RTS.emitDirectiveVariantCC(*HwasanTagMismatchV2Sym);

  const MCSymbolRefExpr *HwasanTagMismatchV2Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV2Sym, OutContext);
  auto Expr = RISCVMCExpr::create(HwasanTagMismatchV2Ref,
                                  RISCVMCExpr::VK_RISCV_CALL, OutContext);

  for (auto &P : HwasanMemaccessSymbols) {
    unsigned Reg = std::get<0>(P.first);
    uint32_t AccessInfo = std::get<1>(P.first);
    MCSymbol *Sym = P.second;

    unsigned Size =
        1 << ((AccessInfo >> HWASanAccessInfo::AccessSizeShift) & 0xf);
    OutStreamer->switchSection(OutContext.getELFSection(
        ".text.hot", ELF::SHT_PROGBITS,
        ELF::SHF_EXECINSTR | ELF::SHF_ALLOC | ELF::SHF_GROUP, 0, Sym->getName(),
        /*IsComdat=*/true));

    OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeFunction);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Hidden);
    OutStreamer->emitLabel(Sym);

    // Extract shadow offset from ptr
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::SLLI).addReg(RISCV::X6).addReg(Reg).addImm(8),
        MCSTI);
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::SRLI)
                                     .addReg(RISCV::X6)
                                     .addReg(RISCV::X6)
                                     .addImm(12),
                                 MCSTI);
    // load shadow tag in X6, X5 contains shadow base
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::ADD)
                                     .addReg(RISCV::X6)
                                     .addReg(RISCV::X5)
                                     .addReg(RISCV::X6),
                                 MCSTI);
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::LBU).addReg(RISCV::X6).addReg(RISCV::X6).addImm(0),
        MCSTI);
    // Extract tag from X5 and compare it with loaded tag from shadow
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::SRLI).addReg(RISCV::X7).addReg(Reg).addImm(56),
        MCSTI);
    MCSymbol *HandleMismatchOrPartialSym = OutContext.createTempSymbol();
    // X7 contains tag from memory, while X6 contains tag from the pointer
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::BNE)
            .addReg(RISCV::X7)
            .addReg(RISCV::X6)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchOrPartialSym,
                                             OutContext)),
        MCSTI);
    MCSymbol *ReturnSym = OutContext.createTempSymbol();
    OutStreamer->emitLabel(ReturnSym);
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::JALR)
                                     .addReg(RISCV::X0)
                                     .addReg(RISCV::X1)
                                     .addImm(0),
                                 MCSTI);
    OutStreamer->emitLabel(HandleMismatchOrPartialSym);

    OutStreamer->emitInstruction(MCInstBuilder(RISCV::ADDI)
                                     .addReg(RISCV::X28)
                                     .addReg(RISCV::X0)
                                     .addImm(16),
                                 MCSTI);
    MCSymbol *HandleMismatchSym = OutContext.createTempSymbol();
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::BGEU)
            .addReg(RISCV::X6)
            .addReg(RISCV::X28)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
        MCSTI);

    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::ANDI).addReg(RISCV::X28).addReg(Reg).addImm(0xF),
        MCSTI);

    if (Size != 1)
      OutStreamer->emitInstruction(MCInstBuilder(RISCV::ADDI)
                                       .addReg(RISCV::X28)
                                       .addReg(RISCV::X28)
                                       .addImm(Size - 1),
                                   MCSTI);
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::BGE)
            .addReg(RISCV::X28)
            .addReg(RISCV::X6)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
        MCSTI);

    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::ORI).addReg(RISCV::X6).addReg(Reg).addImm(0xF),
        MCSTI);
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::LBU).addReg(RISCV::X6).addReg(RISCV::X6).addImm(0),
        MCSTI);
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::BEQ)
            .addReg(RISCV::X6)
            .addReg(RISCV::X7)
            .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
        MCSTI);

    OutStreamer->emitLabel(HandleMismatchSym);

    // | Previous stack frames...        |
    // +=================================+ <-- [SP + 256]
    // |              ...                |
    // |                                 |
    // | Stack frame space for x12 - x31.|
    // |                                 |
    // |              ...                |
    // +---------------------------------+ <-- [SP + 96]
    // | Saved x11(arg1), as             |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 88]
    // | Saved x10(arg0), as             |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 80]
    // |                                 |
    // | Stack frame space for x9.       |
    // +---------------------------------+ <-- [SP + 72]
    // |                                 |
    // | Saved x8(fp), as                |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 64]
    // |              ...                |
    // |                                 |
    // | Stack frame space for x2 - x7.  |
    // |                                 |
    // |              ...                |
    // +---------------------------------+ <-- [SP + 16]
    // | Return address (x1) for caller  |
    // | of __hwasan_check_*.            |
    // +---------------------------------+ <-- [SP + 8]
    // | Reserved place for x0, possibly |
    // | junk, since we don't save it.   |
    // +---------------------------------+ <-- [x2 / SP]

    // Adjust sp
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::ADDI)
                                     .addReg(RISCV::X2)
                                     .addReg(RISCV::X2)
                                     .addImm(-256),
                                 MCSTI);

    // store x10(arg0) by new sp
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::SD)
                                     .addReg(RISCV::X10)
                                     .addReg(RISCV::X2)
                                     .addImm(8 * 10),
                                 MCSTI);
    // store x11(arg1) by new sp
    OutStreamer->emitInstruction(MCInstBuilder(RISCV::SD)
                                     .addReg(RISCV::X11)
                                     .addReg(RISCV::X2)
                                     .addImm(8 * 11),
                                 MCSTI);

    // store x8(fp) by new sp
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::SD).addReg(RISCV::X8).addReg(RISCV::X2).addImm(8 *
                                                                            8),
        MCSTI);
    // store x1(ra) by new sp
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::SD).addReg(RISCV::X1).addReg(RISCV::X2).addImm(1 *
                                                                            8),
        MCSTI);
    if (Reg != RISCV::X10)
      OutStreamer->emitInstruction(MCInstBuilder(RISCV::ADDI)
                                       .addReg(RISCV::X10)
                                       .addReg(Reg)
                                       .addImm(0),
                                   MCSTI);
    OutStreamer->emitInstruction(
        MCInstBuilder(RISCV::ADDI)
            .addReg(RISCV::X11)
            .addReg(RISCV::X0)
            .addImm(AccessInfo & HWASanAccessInfo::RuntimeMask),
        MCSTI);

    OutStreamer->emitInstruction(MCInstBuilder(RISCV::PseudoCALL).addExpr(Expr),
                                 MCSTI);
  }
}

static MCOperand lowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym,
                                    const AsmPrinter &AP) {
  MCContext &Ctx = AP.OutContext;
  RISCVMCExpr::VariantKind Kind;

  switch (MO.getTargetFlags() & ~RISCVII::MO_JUMP_TABLE_BASE) {
  default:
    llvm_unreachable("Unknown target flag on GV operand");
  case RISCVII::MO_None:
    Kind = RISCVMCExpr::VK_RISCV_None;
    break;
  case RISCVII::MO_CALL:
    Kind = RISCVMCExpr::VK_RISCV_CALL;
    break;
  case RISCVII::MO_PLT:
    Kind = RISCVMCExpr::VK_RISCV_CALL_PLT;
    break;
  case RISCVII::MO_LO:
    Kind = RISCVMCExpr::VK_RISCV_LO;
    break;
  case RISCVII::MO_HI:
    Kind = RISCVMCExpr::VK_RISCV_HI;
    break;
  case RISCVII::MO_PCREL_LO:
    Kind = RISCVMCExpr::VK_RISCV_PCREL_LO;
    break;
  case RISCVII::MO_PCREL_HI:
    Kind = RISCVMCExpr::VK_RISCV_PCREL_HI;
    break;
  case RISCVII::MO_GOT_HI:
    Kind = RISCVMCExpr::VK_RISCV_GOT_HI;
    break;
  case RISCVII::MO_TPREL_LO:
    Kind = RISCVMCExpr::VK_RISCV_TPREL_LO;
    break;
  case RISCVII::MO_TPREL_HI:
    Kind = RISCVMCExpr::VK_RISCV_TPREL_HI;
    break;
  case RISCVII::MO_TPREL_ADD:
    Kind = RISCVMCExpr::VK_RISCV_TPREL_ADD;
    break;
  case RISCVII::MO_TLS_GOT_HI:
    Kind = RISCVMCExpr::VK_RISCV_TLS_GOT_HI;
    break;
  case RISCVII::MO_TLS_GD_HI:
    Kind = RISCVMCExpr::VK_RISCV_TLS_GD_HI;
    break;
  case RISCVII::MO_CAPTAB_PCREL_HI:
    Kind = RISCVMCExpr::VK_RISCV_CAPTAB_PCREL_HI;
    break;
  case RISCVII::MO_TPREL_CINCOFFSET:
    Kind = RISCVMCExpr::VK_RISCV_TPREL_CINCOFFSET;
    break;
  case RISCVII::MO_TLS_IE_CAPTAB_PCREL_HI:
    Kind = RISCVMCExpr::VK_RISCV_TLS_IE_CAPTAB_PCREL_HI;
    break;
  case RISCVII::MO_TLS_GD_CAPTAB_PCREL_HI:
    Kind = RISCVMCExpr::VK_RISCV_TLS_GD_CAPTAB_PCREL_HI;
    break;
  case RISCVII::MO_CCALL:
    Kind = RISCVMCExpr::VK_RISCV_CCALL;
    break;
  case RISCVII::MO_CHERIOT_COMPARTMENT_HI:
    Kind = RISCVMCExpr::VK_RISCV_CHERIOT_COMPARTMENT_HI;
    break;
  case RISCVII::MO_CHERIOT_COMPARTMENT_LO_I:
    Kind = RISCVMCExpr::VK_RISCV_CHERIOT_COMPARTMENT_LO_I;
    break;
  case RISCVII::MO_CHERIOT_COMPARTMENT_LO_S:
    Kind = RISCVMCExpr::VK_RISCV_CHERIOT_COMPARTMENT_LO_S;
    break;
  case RISCVII::MO_CHERIOT_COMPARTMENT_SIZE:
    Kind = RISCVMCExpr::VK_RISCV_CHERIOT_COMPARTMENT_SIZE;
    break;
  }

  const MCExpr *ME =
      MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_None, Ctx);

  if (!MO.isJTI() && !MO.isMBB() && MO.getOffset())
    ME = MCBinaryExpr::createAdd(
        ME, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);

  if (Kind != RISCVMCExpr::VK_RISCV_None)
    ME = RISCVMCExpr::create(ME, Kind, Ctx);
  return MCOperand::createExpr(ME);
}

bool RISCVAsmPrinter::lowerOperand(const MachineOperand &MO,
                                   MCOperand &MCOp) const {
  switch (MO.getType()) {
  default:
    report_fatal_error("lowerOperand: unknown operand type");
  case MachineOperand::MO_Register:
    // Ignore all implicit register operands.
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    break;
  case MachineOperand::MO_RegisterMask:
    // Regmasks are like implicit defs.
    return false;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    break;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = lowerSymbolOperand(MO, MO.getMBB()->getSymbol(), *this);
    break;
  case MachineOperand::MO_GlobalAddress: {
    const GlobalValue *GV = MO.getGlobal();
    MCSymbol *Sym;
    if (MO.getTargetFlags() & RISCVII::MO_JUMP_TABLE_BASE)
      Sym = getSymbolWithGlobalValueBase(GV, "$jump_table_base");
    else
      Sym = getSymbolPreferLocal(*GV);
    MCOp = lowerSymbolOperand(MO, Sym, *this);
    break;
  }
  case MachineOperand::MO_BlockAddress:
    MCOp = lowerSymbolOperand(MO, GetBlockAddressSymbol(MO.getBlockAddress()),
                              *this);
    break;
  case MachineOperand::MO_ExternalSymbol:
    MCOp = lowerSymbolOperand(MO, GetExternalSymbolSymbol(MO.getSymbolName()),
                              *this);
    break;
  case MachineOperand::MO_ConstantPoolIndex:
    MCOp = lowerSymbolOperand(MO, GetCPISymbol(MO.getIndex()), *this);
    break;
  case MachineOperand::MO_JumpTableIndex:
    MCOp = lowerSymbolOperand(MO, GetJTISymbol(MO.getIndex()), *this);
    break;
  case MachineOperand::MO_MCSymbol:
    MCOp = lowerSymbolOperand(MO, MO.getMCSymbol(), *this);
    break;
  }
  return true;
}

static bool lowerRISCVVMachineInstrToMCInst(const MachineInstr *MI,
                                            MCInst &OutMI) {
  const RISCVVPseudosTable::PseudoInfo *RVV =
      RISCVVPseudosTable::getPseudoInfo(MI->getOpcode());
  if (!RVV)
    return false;

  OutMI.setOpcode(RVV->BaseInstr);

  const MachineBasicBlock *MBB = MI->getParent();
  assert(MBB && "MI expected to be in a basic block");
  const MachineFunction *MF = MBB->getParent();
  assert(MF && "MBB expected to be in a machine function");

  const RISCVSubtarget &Subtarget = MF->getSubtarget<RISCVSubtarget>();
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  const TargetRegisterInfo *TRI = Subtarget.getRegisterInfo();
  assert(TRI && "TargetRegisterInfo expected");

  const MCInstrDesc &MCID = MI->getDesc();
  uint64_t TSFlags = MCID.TSFlags;
  unsigned NumOps = MI->getNumExplicitOperands();

  // Skip policy, VL and SEW operands which are the last operands if present.
  if (RISCVII::hasVecPolicyOp(TSFlags))
    --NumOps;
  if (RISCVII::hasVLOp(TSFlags))
    --NumOps;
  if (RISCVII::hasSEWOp(TSFlags))
    --NumOps;
  if (RISCVII::hasRoundModeOp(TSFlags))
    --NumOps;

  bool hasVLOutput = RISCV::isFaultFirstLoad(*MI);
  for (unsigned OpNo = 0; OpNo != NumOps; ++OpNo) {
    const MachineOperand &MO = MI->getOperand(OpNo);
    // Skip vl ouput. It should be the second output.
    if (hasVLOutput && OpNo == 1)
      continue;

    // Skip merge op. It should be the first operand after the defs.
    if (OpNo == MI->getNumExplicitDefs() && MO.isReg() && MO.isTied()) {
      assert(MCID.getOperandConstraint(OpNo, MCOI::TIED_TO) == 0 &&
             "Expected tied to first def.");
      const MCInstrDesc &OutMCID = TII->get(OutMI.getOpcode());
      // Skip if the next operand in OutMI is not supposed to be tied. Unless it
      // is a _TIED instruction.
      if (OutMCID.getOperandConstraint(OutMI.getNumOperands(), MCOI::TIED_TO) <
              0 &&
          !RISCVII::isTiedPseudo(TSFlags))
        continue;
    }

    MCOperand MCOp;
    switch (MO.getType()) {
    default:
      llvm_unreachable("Unknown operand type");
    case MachineOperand::MO_Register: {
      Register Reg = MO.getReg();

      if (RISCV::VRM2RegClass.contains(Reg) ||
          RISCV::VRM4RegClass.contains(Reg) ||
          RISCV::VRM8RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_vrm1_0);
        assert(Reg && "Subregister does not exist");
      } else if (RISCV::FPR16RegClass.contains(Reg)) {
        Reg =
            TRI->getMatchingSuperReg(Reg, RISCV::sub_16, &RISCV::FPR32RegClass);
        assert(Reg && "Subregister does not exist");
      } else if (RISCV::FPR64RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_32);
        assert(Reg && "Superregister does not exist");
      } else if (RISCV::VRN2M1RegClass.contains(Reg) ||
                 RISCV::VRN2M2RegClass.contains(Reg) ||
                 RISCV::VRN2M4RegClass.contains(Reg) ||
                 RISCV::VRN3M1RegClass.contains(Reg) ||
                 RISCV::VRN3M2RegClass.contains(Reg) ||
                 RISCV::VRN4M1RegClass.contains(Reg) ||
                 RISCV::VRN4M2RegClass.contains(Reg) ||
                 RISCV::VRN5M1RegClass.contains(Reg) ||
                 RISCV::VRN6M1RegClass.contains(Reg) ||
                 RISCV::VRN7M1RegClass.contains(Reg) ||
                 RISCV::VRN8M1RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_vrm1_0);
        assert(Reg && "Subregister does not exist");
      }

      MCOp = MCOperand::createReg(Reg);
      break;
    }
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      break;
    }
    OutMI.addOperand(MCOp);
  }

  // Unmasked pseudo instructions need to append dummy mask operand to
  // V instructions. All V instructions are modeled as the masked version.
  const MCInstrDesc &OutMCID = TII->get(OutMI.getOpcode());
  if (OutMI.getNumOperands() < OutMCID.getNumOperands()) {
    assert(OutMCID.operands()[OutMI.getNumOperands()].RegClass ==
               RISCV::VMV0RegClassID &&
           "Expected only mask operand to be missing");
    OutMI.addOperand(MCOperand::createReg(RISCV::NoRegister));
  }

  assert(OutMI.getNumOperands() == OutMCID.getNumOperands());
  return true;
}

bool RISCVAsmPrinter::lowerToMCInst(const MachineInstr *MI, MCInst &OutMI) {
  if (lowerRISCVVMachineInstrToMCInst(MI, OutMI))
    return false;

  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    if (lowerOperand(MO, MCOp))
      OutMI.addOperand(MCOp);
  }

  switch (OutMI.getOpcode()) {
  case TargetOpcode::PATCHABLE_FUNCTION_ENTER: {
    const Function &F = MI->getParent()->getParent()->getFunction();
    if (F.hasFnAttribute("patchable-function-entry")) {
      unsigned Num;
      if (F.getFnAttribute("patchable-function-entry")
              .getValueAsString()
              .getAsInteger(10, Num))
        return false;
      emitNops(Num);
      return true;
    }
    break;
  }
  }
  return false;
}
