//===- FuzzerTraceState.cpp - Trace-based fuzzer mutator ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This file implements a mutation algorithm based on instruction traces and
// on taint analysis feedback from DFSan.
//
// Instruction traces are special hooks inserted by the compiler around
// interesting instructions. Currently supported traces:
//   * __sanitizer_cov_trace_cmp -- inserted before every ICMP instruction,
//    receives the type, size and arguments of ICMP.
//
// Every time a traced event is intercepted we analyse the data involved
// in the event and suggest a mutation for future executions.
// For example if 4 bytes of data that derive from input bytes {4,5,6,7}
// are compared with a constant 12345,
// we try to insert 12345, 12344, 12346 into bytes
// {4,5,6,7} of the next fuzzed inputs.
//
// The fuzzer can work only with the traces, or with both traces and DFSan.
//
// DataFlowSanitizer (DFSan) is a tool for
// generalised dynamic data flow (taint) analysis:
// http://clang.llvm.org/docs/DataFlowSanitizer.html .
//
// The approach with DFSan-based fuzzing has some similarity to
// "Taint-based Directed Whitebox Fuzzing"
// by Vijay Ganesh & Tim Leek & Martin Rinard:
// http://dspace.mit.edu/openaccess-disseminate/1721.1/59320,
// but it uses a full blown LLVM IR taint analysis and separate instrumentation
// to analyze all of the "attack points" at once.
//
// Workflow with DFSan:
//   * lib/Fuzzer/Fuzzer*.cpp is compiled w/o any instrumentation.
//   * The code under test is compiled with DFSan *and* with instruction traces.
//   * Every call to HOOK(a,b) is replaced by DFSan with
//     __dfsw_HOOK(a, b, label(a), label(b)) so that __dfsw_HOOK
//     gets all the taint labels for the arguments.
//   * At the Fuzzer startup we assign a unique DFSan label
//     to every byte of the input string (Fuzzer::CurrentUnit) so that for any
//     chunk of data we know which input bytes it has derived from.
//   * The __dfsw_* functions (implemented in this file) record the
//     parameters (i.e. the application data and the corresponding taint labels)
//     in a global state.
//   * Fuzzer::ApplyTraceBasedMutation() tries to use the data recorded
//     by __dfsw_* hooks to guide the fuzzing towards new application states.
//
// Parts of this code will not function when DFSan is not linked in.
// Instead of using ifdefs and thus requiring a separate build of lib/Fuzzer
// we redeclare the dfsan_* interface functions as weak and check if they
// are nullptr before calling.
// If this approach proves to be useful we may add attribute(weak) to the
// dfsan declarations in dfsan_interface.h
//
// This module is in the "proof of concept" stage.
// It is capable of solving only the simplest puzzles
// like test/dfsan/DFSanSimpleCmpTest.cpp.
//===----------------------------------------------------------------------===//

/* Example of manual usage (-fsanitize=dataflow is optional):
(
  cd $LLVM/lib/Fuzzer/
  clang  -fPIC -c -g -O2 -std=c++11 Fuzzer*.cpp
  clang++ -O0 -std=c++11 -fsanitize-coverage=edge,trace-cmp \
    -fsanitize=dataflow \
    test/dfsan/DFSanSimpleCmpTest.cpp Fuzzer*.o
  ./a.out
)
*/

#include "FuzzerInternal.h"
#include <sanitizer/dfsan_interface.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

extern "C" {
__attribute__((weak))
dfsan_label dfsan_create_label(const char *desc, void *userdata);
__attribute__((weak))
void dfsan_set_label(dfsan_label label, void *addr, size_t size);
__attribute__((weak))
void dfsan_add_label(dfsan_label label, void *addr, size_t size);
__attribute__((weak))
const struct dfsan_label_info *dfsan_get_label_info(dfsan_label label);
__attribute__((weak))
dfsan_label dfsan_read_label(const void *addr, size_t size);
}  // extern "C"

namespace fuzzer {

static bool ReallyHaveDFSan() {
  return &dfsan_create_label != nullptr;
}

// These values are copied from include/llvm/IR/InstrTypes.h.
// We do not include the LLVM headers here to remain independent.
// If these values ever change, an assertion in ComputeCmp will fail.
enum Predicate {
  ICMP_EQ = 32,  ///< equal
  ICMP_NE = 33,  ///< not equal
  ICMP_UGT = 34, ///< unsigned greater than
  ICMP_UGE = 35, ///< unsigned greater or equal
  ICMP_ULT = 36, ///< unsigned less than
  ICMP_ULE = 37, ///< unsigned less or equal
  ICMP_SGT = 38, ///< signed greater than
  ICMP_SGE = 39, ///< signed greater or equal
  ICMP_SLT = 40, ///< signed less than
  ICMP_SLE = 41, ///< signed less or equal
};

template <class U, class S>
bool ComputeCmp(size_t CmpType, U Arg1, U Arg2) {
  switch(CmpType) {
    case ICMP_EQ : return Arg1 == Arg2;
    case ICMP_NE : return Arg1 != Arg2;
    case ICMP_UGT: return Arg1 > Arg2;
    case ICMP_UGE: return Arg1 >= Arg2;
    case ICMP_ULT: return Arg1 < Arg2;
    case ICMP_ULE: return Arg1 <= Arg2;
    case ICMP_SGT: return (S)Arg1 > (S)Arg2;
    case ICMP_SGE: return (S)Arg1 >= (S)Arg2;
    case ICMP_SLT: return (S)Arg1 < (S)Arg2;
    case ICMP_SLE: return (S)Arg1 <= (S)Arg2;
    default: assert(0 && "unsupported CmpType");
  }
  return false;
}

static bool ComputeCmp(size_t CmpSize, size_t CmpType, uint64_t Arg1,
                       uint64_t Arg2) {
  if (CmpSize == 8) return ComputeCmp<uint64_t, int64_t>(CmpType, Arg1, Arg2);
  if (CmpSize == 4) return ComputeCmp<uint32_t, int32_t>(CmpType, Arg1, Arg2);
  if (CmpSize == 2) return ComputeCmp<uint16_t, int16_t>(CmpType, Arg1, Arg2);
  if (CmpSize == 1) return ComputeCmp<uint8_t, int8_t>(CmpType, Arg1, Arg2);
  // Other size, ==
  if (CmpType == ICMP_EQ) return Arg1 == Arg2;
  // assert(0 && "unsupported cmp and type size combination");
  return true;
}

// As a simplification we use the range of input bytes instead of a set of input
// bytes.
struct LabelRange {
  uint16_t Beg, End;  // Range is [Beg, End), thus Beg==End is an empty range.

  LabelRange(uint16_t Beg = 0, uint16_t End = 0) : Beg(Beg), End(End) {}

  static LabelRange Join(LabelRange LR1, LabelRange LR2) {
    if (LR1.Beg == LR1.End) return LR2;
    if (LR2.Beg == LR2.End) return LR1;
    return {std::min(LR1.Beg, LR2.Beg), std::max(LR1.End, LR2.End)};
  }
  LabelRange &Join(LabelRange LR) {
    return *this = Join(*this, LR);
  }
  static LabelRange Singleton(const dfsan_label_info *LI) {
    uint16_t Idx = (uint16_t)(uintptr_t)LI->userdata;
    assert(Idx > 0);
    return {(uint16_t)(Idx - 1), Idx};
  }
};

// A passport for a CMP site. We want to keep track of where the given CMP is
// and how many times it is evaluated to true or false.
struct CmpSitePassport {
  uintptr_t PC;
  size_t Counter[2];

  bool IsInterestingCmpTarget() {
    static const size_t kRareEnough = 50;
    size_t C0 = Counter[0];
    size_t C1 = Counter[1];
    return C0 > kRareEnough * (C1 + 1) || C1 > kRareEnough * (C0 + 1);
  }
};

// For now, just keep a simple imprecise hash table PC => CmpSitePassport.
// Potentially, will need to have a compiler support to have a precise mapping
// and also thread-safety.
struct CmpSitePassportTable {
  static const size_t kSize = 99991;  // Prime.
  CmpSitePassport Passports[kSize];

  CmpSitePassport *GetPassport(uintptr_t PC) {
    uintptr_t Idx = PC & kSize;
    CmpSitePassport *Res = &Passports[Idx];
    if (Res->PC == 0)  // Not thread safe.
      Res->PC = PC;
    return Res->PC == PC ? Res : nullptr;
  }
};

static CmpSitePassportTable CSPTable;  // Zero initialized.

// For now, very simple: put Size bytes of Data at position Pos.
struct TraceBasedMutation {
  size_t Pos;
  size_t Size;
  uint64_t Data;
};

class TraceState {
 public:
   TraceState(const Fuzzer::FuzzingOptions &Options, const Unit &CurrentUnit)
       : Options(Options), CurrentUnit(CurrentUnit) {}

  LabelRange GetLabelRange(dfsan_label L);
  void DFSanCmpCallback(uintptr_t PC, size_t CmpSize, size_t CmpType,
                        uint64_t Arg1, uint64_t Arg2, dfsan_label L1,
                        dfsan_label L2);
  void DFSanSwitchCallback(uint64_t PC, size_t ValSizeInBits, uint64_t Val,
                           size_t NumCases, uint64_t *Cases, dfsan_label L);
  void TraceCmpCallback(uintptr_t PC, size_t CmpSize, size_t CmpType,
                        uint64_t Arg1, uint64_t Arg2);

  void TraceSwitchCallback(uintptr_t PC, size_t ValSizeInBits, uint64_t Val,
                           size_t NumCases, uint64_t *Cases);
  int TryToAddDesiredData(uint64_t PresentData, uint64_t DesiredData,
                           size_t DataSize);

  void StartTraceRecording() {
    if (!Options.UseTraces) return;
    RecordingTraces = true;
    Mutations.clear();
  }

  size_t StopTraceRecording(FuzzerRandomBase &Rand) {
    RecordingTraces = false;
    return Mutations.size();
  }

  void ApplyTraceBasedMutation(size_t Idx, fuzzer::Unit *U);

 private:
  bool IsTwoByteData(uint64_t Data) {
    int64_t Signed = static_cast<int64_t>(Data);
    Signed >>= 16;
    return Signed == 0 || Signed == -1L;
  }
  bool RecordingTraces = false;
  std::vector<TraceBasedMutation> Mutations;
  LabelRange LabelRanges[1 << (sizeof(dfsan_label) * 8)] = {};
  const Fuzzer::FuzzingOptions &Options;
  const Unit &CurrentUnit;
};

LabelRange TraceState::GetLabelRange(dfsan_label L) {
  LabelRange &LR = LabelRanges[L];
  if (LR.Beg < LR.End || L == 0)
    return LR;
  const dfsan_label_info *LI = dfsan_get_label_info(L);
  if (LI->l1 || LI->l2)
    return LR = LabelRange::Join(GetLabelRange(LI->l1), GetLabelRange(LI->l2));
  return LR = LabelRange::Singleton(LI);
}

void TraceState::ApplyTraceBasedMutation(size_t Idx, fuzzer::Unit *U) {
  assert(Idx < Mutations.size());
  auto &M = Mutations[Idx];
  if (Options.Verbosity >= 3)
    Printf("TBM %zd %zd %zd\n", M.Pos, M.Size, M.Data);
  if (M.Pos + M.Size > U->size()) return;
  memcpy(U->data() + M.Pos, &M.Data, M.Size);
}

void TraceState::DFSanCmpCallback(uintptr_t PC, size_t CmpSize, size_t CmpType,
                                  uint64_t Arg1, uint64_t Arg2, dfsan_label L1,
                                  dfsan_label L2) {
  assert(ReallyHaveDFSan());
  if (!RecordingTraces) return;
  if (L1 == 0 && L2 == 0)
    return;  // Not actionable.
  if (L1 != 0 && L2 != 0)
    return;  // Probably still actionable.
  bool Res = ComputeCmp(CmpSize, CmpType, Arg1, Arg2);
  uint64_t Data = L1 ? Arg2 : Arg1;
  LabelRange LR = L1 ? GetLabelRange(L1) : GetLabelRange(L2);

  for (size_t Pos = LR.Beg; Pos + CmpSize <= LR.End; Pos++) {
    Mutations.push_back({Pos, CmpSize, Data});
    Mutations.push_back({Pos, CmpSize, Data + 1});
    Mutations.push_back({Pos, CmpSize, Data - 1});
  }

  if (CmpSize > LR.End - LR.Beg)
    Mutations.push_back({LR.Beg, (unsigned)(LR.End - LR.Beg), Data});


  if (Options.Verbosity >= 3)
    Printf("DFSanCmpCallback: PC %lx S %zd T %zd A1 %llx A2 %llx R %d L1 %d L2 "
           "%d MU %zd\n",
           PC, CmpSize, CmpType, Arg1, Arg2, Res, L1, L2, Mutations.size());
}

void TraceState::DFSanSwitchCallback(uint64_t PC, size_t ValSizeInBits,
                                     uint64_t Val, size_t NumCases,
                                     uint64_t *Cases, dfsan_label L) {
  assert(ReallyHaveDFSan());
  if (!RecordingTraces) return;
  if (!L) return;  // Not actionable.
  LabelRange LR = GetLabelRange(L);
  size_t ValSize = ValSizeInBits / 8;
  bool TryShort = IsTwoByteData(Val);
  for (size_t i = 0; i < NumCases; i++)
    TryShort &= IsTwoByteData(Cases[i]);

  for (size_t Pos = LR.Beg; Pos + ValSize <= LR.End; Pos++)
    for (size_t i = 0; i < NumCases; i++)
      Mutations.push_back({Pos, ValSize, Cases[i]});

  if (TryShort)
    for (size_t Pos = LR.Beg; Pos + 2 <= LR.End; Pos++)
      for (size_t i = 0; i < NumCases; i++)
        Mutations.push_back({Pos, 2, Cases[i]});

  if (Options.Verbosity >= 3)
    Printf("DFSanSwitchCallback: PC %lx Val %zd SZ %zd # %zd L %d: {%d, %d} "
           "TryShort %d\n",
           PC, Val, ValSize, NumCases, L, LR.Beg, LR.End, TryShort);
}

int TraceState::TryToAddDesiredData(uint64_t PresentData, uint64_t DesiredData,
                                    size_t DataSize) {
  int Res = 0;
  const uint8_t *Beg = CurrentUnit.data();
  const uint8_t *End = Beg + CurrentUnit.size();
  for (const uint8_t *Cur = Beg; Cur < End; Cur++) {
    Cur = (uint8_t *)memmem(Cur, End - Cur, &PresentData, DataSize);
    if (!Cur)
      break;
    size_t Pos = Cur - Beg;
    assert(Pos < CurrentUnit.size());
    if (Mutations.size() > 100000U) return Res;  // Just in case.
    Mutations.push_back({Pos, DataSize, DesiredData});
    Mutations.push_back({Pos, DataSize, DesiredData + 1});
    Mutations.push_back({Pos, DataSize, DesiredData - 1});
    Res++;
  }
  return Res;
}

void TraceState::TraceCmpCallback(uintptr_t PC, size_t CmpSize, size_t CmpType,
                                  uint64_t Arg1, uint64_t Arg2) {
  if (!RecordingTraces) return;
  int Added = 0;
  CmpSitePassport *CSP = CSPTable.GetPassport(PC);
  if (!CSP) return;
  CSP->Counter[ComputeCmp(CmpSize, CmpType, Arg1, Arg2)]++;
  size_t C0 = CSP->Counter[0];
  size_t C1 = CSP->Counter[1];
  // FIXME: is this a good idea or a bad?
  // if (!CSP->IsInterestingCmpTarget())
  //  return;
  if (Options.Verbosity >= 3)
    Printf("TraceCmp: %p %zd/%zd; %zd %zd\n", CSP->PC, C0, C1, Arg1, Arg2);
  Added += TryToAddDesiredData(Arg1, Arg2, CmpSize);
  Added += TryToAddDesiredData(Arg2, Arg1, CmpSize);
  if (!Added && CmpSize == 4 && IsTwoByteData(Arg1) && IsTwoByteData(Arg2)) {
    Added += TryToAddDesiredData(Arg1, Arg2, 2);
    Added += TryToAddDesiredData(Arg2, Arg1, 2);
  }
}

void TraceState::TraceSwitchCallback(uintptr_t PC, size_t ValSizeInBits,
                                     uint64_t Val, size_t NumCases,
                                     uint64_t *Cases) {
  if (!RecordingTraces) return;
  size_t ValSize = ValSizeInBits / 8;
  bool TryShort = IsTwoByteData(Val);
  for (size_t i = 0; i < NumCases; i++)
    TryShort &= IsTwoByteData(Cases[i]);

  if (Options.Verbosity >= 3)
    Printf("TraceSwitch: %p %zd # %zd; TryShort %d\n", PC, Val, NumCases,
           TryShort);

  for (size_t i = 0; i < NumCases; i++) {
    TryToAddDesiredData(Val, Cases[i], ValSize);
    if (TryShort)
      TryToAddDesiredData(Val, Cases[i], 2);
  }

}

static TraceState *TS;

void Fuzzer::StartTraceRecording() {
  if (!TS) return;
  if (ReallyHaveDFSan())
    for (size_t i = 0; i < static_cast<size_t>(Options.MaxLen); i++)
      dfsan_set_label(i + 1, &CurrentUnit[i], 1);
  TS->StartTraceRecording();
}

size_t Fuzzer::StopTraceRecording() {
  if (!TS) return 0;
  return TS->StopTraceRecording(USF.GetRand());
}

void Fuzzer::ApplyTraceBasedMutation(size_t Idx, Unit *U) {
  assert(TS);
  TS->ApplyTraceBasedMutation(Idx, U);
}

void Fuzzer::InitializeTraceState() {
  if (!Options.UseTraces) return;
  TS = new TraceState(Options, CurrentUnit);
  CurrentUnit.resize(Options.MaxLen);
  // The rest really requires DFSan.
  if (!ReallyHaveDFSan()) return;
  for (size_t i = 0; i < static_cast<size_t>(Options.MaxLen); i++) {
    dfsan_label L = dfsan_create_label("input", (void*)(i + 1));
    // We assume that no one else has called dfsan_create_label before.
    if (L != i + 1) {
      Printf("DFSan labels are not starting from 1, exiting\n");
      exit(1);
    }
  }
}

static size_t InternalStrnlen(const char *S, size_t MaxLen) {
  size_t Len = 0;
  for (; Len < MaxLen && S[Len]; Len++) {}
  return Len;
}

}  // namespace fuzzer

using fuzzer::TS;

extern "C" {
void __dfsw___sanitizer_cov_trace_cmp(uint64_t SizeAndType, uint64_t Arg1,
                                      uint64_t Arg2, dfsan_label L0,
                                      dfsan_label L1, dfsan_label L2) {
  if (!TS) return;
  assert(L0 == 0);
  uintptr_t PC = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  uint64_t CmpSize = (SizeAndType >> 32) / 8;
  uint64_t Type = (SizeAndType << 32) >> 32;
  TS->DFSanCmpCallback(PC, CmpSize, Type, Arg1, Arg2, L1, L2);
}

void __dfsw___sanitizer_cov_trace_switch(uint64_t Val, uint64_t *Cases,
                                         dfsan_label L1, dfsan_label L2) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  TS->DFSanSwitchCallback(PC, Cases[1], Val, Cases[0], Cases+2, L1);
}

void dfsan_weak_hook_memcmp(void *caller_pc, const void *s1, const void *s2,
                            size_t n, dfsan_label s1_label,
                            dfsan_label s2_label, dfsan_label n_label) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(n, sizeof(S1)));
  memcpy(&S2, s2, std::min(n, sizeof(S2)));
  dfsan_label L1 = dfsan_read_label(s1, n);
  dfsan_label L2 = dfsan_read_label(s2, n);
  TS->DFSanCmpCallback(PC, n, fuzzer::ICMP_EQ, S1, S2, L1, L2);
}

void dfsan_weak_hook_strncmp(void *caller_pc, const char *s1, const char *s2,
                             size_t n, dfsan_label s1_label,
                             dfsan_label s2_label, dfsan_label n_label) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  n = std::min(n, fuzzer::InternalStrnlen(s1, n));
  n = std::min(n, fuzzer::InternalStrnlen(s2, n));
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(n, sizeof(S1)));
  memcpy(&S2, s2, std::min(n, sizeof(S2)));
  dfsan_label L1 = dfsan_read_label(s1, n);
  dfsan_label L2 = dfsan_read_label(s2, n);
  TS->DFSanCmpCallback(PC, n, fuzzer::ICMP_EQ, S1, S2, L1, L2);
}

void dfsan_weak_hook_strcmp(void *caller_pc, const char *s1, const char *s2,
                            dfsan_label s1_label, dfsan_label s2_label) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  size_t Len1 = strlen(s1);
  size_t Len2 = strlen(s2);
  size_t N = std::min(Len1, Len2);
  if (N <= 1) return;  // Not interesting.
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(N, sizeof(S1)));
  memcpy(&S2, s2, std::min(N, sizeof(S2)));
  dfsan_label L1 = dfsan_read_label(s1, Len1);
  dfsan_label L2 = dfsan_read_label(s2, Len2);
  TS->DFSanCmpCallback(PC, N, fuzzer::ICMP_EQ, S1, S2, L1, L2);
}

void __sanitizer_weak_hook_memcmp(void *caller_pc, const void *s1,
                                  const void *s2, size_t n) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(n, sizeof(S1)));
  memcpy(&S2, s2, std::min(n, sizeof(S2)));
  TS->TraceCmpCallback(PC, n, fuzzer::ICMP_EQ, S1, S2);
}

void __sanitizer_weak_hook_strncmp(void *caller_pc, const char *s1,
                                   const char *s2, size_t n) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  size_t Len1 = fuzzer::InternalStrnlen(s1, n);
  size_t Len2 = fuzzer::InternalStrnlen(s2, n);
  n = std::min(n, Len1);
  n = std::min(n, Len2);
  if (n <= 1) return;  // Not interesting.
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(n, sizeof(S1)));
  memcpy(&S2, s2, std::min(n, sizeof(S2)));
  TS->TraceCmpCallback(PC, n, fuzzer::ICMP_EQ, S1, S2);
}

void __sanitizer_weak_hook_strcmp(void *caller_pc, const char *s1,
                                   const char *s2) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(caller_pc);
  uint64_t S1 = 0, S2 = 0;
  size_t Len1 = strlen(s1);
  size_t Len2 = strlen(s2);
  size_t N = std::min(Len1, Len2);
  if (N <= 1) return;  // Not interesting.
  // Simplification: handle only first 8 bytes.
  memcpy(&S1, s1, std::min(N, sizeof(S1)));
  memcpy(&S2, s2, std::min(N, sizeof(S2)));
  TS->TraceCmpCallback(PC, N, fuzzer::ICMP_EQ, S1, S2);
}


void __sanitizer_cov_trace_cmp(uint64_t SizeAndType, uint64_t Arg1,
                               uint64_t Arg2) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  uint64_t CmpSize = (SizeAndType >> 32) / 8;
  uint64_t Type = (SizeAndType << 32) >> 32;
  TS->TraceCmpCallback(PC, CmpSize, Type, Arg1, Arg2);
}

void __sanitizer_cov_trace_switch(uint64_t Val, uint64_t *Cases) {
  if (!TS) return;
  uintptr_t PC = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  TS->TraceSwitchCallback(PC, Cases[1], Val, Cases[0], Cases + 2);
}

}  // extern "C"
