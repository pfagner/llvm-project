//===- CXXCopyElision.cpp - Eliminate calls of c++ copy/move ctors ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// TODO
// 1. Add description of pass
// 2. Handle different alloca types (create bitcast)
// 3. Handle invoke instr
// 4. Use DOM tree?
// 5. Don't apply for volatile objects
// 6. Add comments and make cleanup
//===----------------------------------------------------------------------===//

#include <llvm/Analysis/CFG.h>
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include <llvm/IR/InstrTypes.h>

using namespace llvm;

#define DEBUG_TYPE "cxx_copy_elision"

namespace {

bool isCxxCMCtor(const CallBase& CB) {
  return CB.isCxxCMCtorOrDtor() && (CB.getNumArgOperands() == 2);
}

bool isCxxDtor(const CallBase& CB) {
  return CB.isCxxCMCtorOrDtor() && (CB.getNumArgOperands() == 1);
}

bool isCxxDtor(const Instruction& I) {
  if (auto* CB = dyn_cast<CallBase>(&I)) {
    return isCxxDtor(*CB);
  }
  return false;
}

bool isCxxCMCtorOrDtor(const CallBase& CB) {
  return isCxxCMCtor(CB) || isCxxDtor(CB);
}

class CXXCopyElisionLegacyPass;

using CtorVector = SmallVector<CallBase *, 32>;

class CtorVisitor : public InstVisitor<CtorVisitor> {
public:
  explicit CtorVisitor(Function& F, CtorVector& CtorVec)
      : F(F), CtorVec(CtorVec) {}

  void collectCtors() {
    for (auto &BB : F) {
      visit(BB);
    }
  }

  void visitCallBase(CallBase& CB) {
    // collect all calls/invokes without uses
    // TODO: need additional checks for invoke
    if (isCxxCMCtor(CB))
      CtorVec.push_back(&CB);
  }

private:
  Function& F;
  CtorVector& CtorVec;
};

class CXXCopyElisionLegacyPass : public FunctionPass {
public:
  static char ID;
  CXXCopyElisionLegacyPass() : FunctionPass(ID) {
    initializeCXXCopyElisionLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    CtorVector CtorVec;
    CtorVisitor CV(F, CtorVec);

    CV.collectCtors();

    LLVM_DEBUG(dbgs() << "===================================="
                      << "\n*** Function*** : " << F.getName() << "\n");

    bool Changed = false;
    for (auto &Call : CtorVec) {
      if (canCtorBeElided(*Call)) {
        DeadInstList.push_back(Call);

        while (!DeadInstList.empty()) {
          auto *EI = DeadInstList.pop_back_val();

          LLVM_DEBUG(dbgs() << "*** Erase Inst *** : " << *EI << "\n");
          assert(!EI->getNumUses() && "Erased instruction has uses");

          EI->eraseFromParent();
        }

        LLVM_DEBUG(dbgs() << "*** Replace Inst (From) *** : " << *From
                          << "\n*** With (To) *** : " << *To << "\n");
        From->replaceAllUsesWith(To);
        if (!From->getNumUses())
          From->eraseFromParent();

        Changed = true;
      }
    }

    LLVM_DEBUG(dbgs() << "====================================\n");

    return Changed;
  }

private:
  bool canCtorBeElided(CallBase &Ctor) {
    const auto &DL = Ctor.getFunction()->getParent()->getDataLayout();

    auto *AllocTo = GetUnderlyingObject(Ctor.getOperand(0), DL);
    auto *AllocFrom = GetUnderlyingObject(Ctor.getOperand(1), DL);
    auto *ImmediateFrom = Ctor.getOperand(1);

    LLVM_DEBUG(dbgs() << "*** Ctor *** : " << Ctor
                      << "\n*** AllocTo *** : " << *AllocTo
                      << "\n*** AllocaFrom *** : " << *AllocFrom
                      << "\n*** ImmFrom *** : " << *ImmediateFrom << "\n");

    // consider only automatic variables
    if (!isa<AllocaInst>(AllocTo) || !isa<AllocaInst>(AllocFrom))
      return false;

    auto ImmDtors =
        findImmediateCopiedDtors(cast<Instruction>(*ImmediateFrom));

    SmallVector<Instruction *, 32> InstList;
    for (auto* U : AllocFrom->users()) {
      auto *I = dyn_cast<Instruction>(U);
      LLVM_DEBUG(dbgs() << "*** User *** : " << *U << "\n");

      if (!I)
        return false;

      if (I == &Ctor)
        continue;

      // remove all trivial instructions later
      if (isTrivialInstruction(*I)) {
        InstList.push_back(I);
        for (auto* IU : I->users())
          InstList.push_back(cast<Instruction>(IU));
        continue;
      }

      if (isPotentiallyReachable(&Ctor, I)) {
        bool CanBeElided = false;

        for (const auto* Dtor : ImmDtors) {
          assert(isPotentiallyReachable(&Ctor, Dtor) &&
                 "dtor is not reached by copy/move ctor");
          CanBeElided = isPotentiallyReachable(Dtor, I);
          if (!CanBeElided)
            break;
        }

        if (!CanBeElided)
          return false;
      }
    }

    From = cast<Instruction>(AllocFrom);
    To = cast<Instruction>(AllocTo);
    DeadInstList = std::move(InstList);

#ifndef NDEBUG
    for (const auto* DI : DeadInstList)
      LLVM_DEBUG(dbgs() << "*** Dead Inst *** : " << *DI << "\n");
#endif

    return true;
  }

  bool isTrivialInstruction(const Instruction &I) {
    if (auto *II = dyn_cast<IntrinsicInst>(&I))
      if (!II->isLifetimeStartOrEnd())
        return false;

    if (isCxxDtor(I))
      return true;

    return (isa<BitCastInst>(I) || isa<GetElementPtrInst>(I)) &&
           onlyUsedByLifetimeMarkers(&I);
  }

  SmallVector<const Instruction*, 2>
  findImmediateCopiedDtors(const Instruction& I) {
    SmallVector<const Instruction*, 2> Dtors;

    for (auto* IU : I.users()) {
      auto* DI = dyn_cast<Instruction>(IU);

      if (!DI)
        continue;

      if (isCxxDtor(*DI)) {
        Dtors.push_back(DI);
      } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::lifetime_end)
          Dtors.push_back(DI);
      } else if ((isa<BitCastInst>(DI) || isa<GetElementPtrInst>(DI))
                 && DI->hasOneUse()) {
        auto U = DI->users().begin();
        if (auto* UII = dyn_cast<IntrinsicInst>(*U)) {
          if (II->getIntrinsicID() == Intrinsic::lifetime_end)
            Dtors.push_back(DI);
        } else if (auto* UI = dyn_cast<Instruction>(*U)) {
          if (isCxxDtor(*UI))
            Dtors.push_back(DI);
        }
      }
    }

    return Dtors;
  }

  SmallVector<Instruction *, 32> DeadInstList;
  Instruction *From = nullptr;
  Instruction *To = nullptr;
};

} // end anonymous namespace

char CXXCopyElisionLegacyPass::ID = 0;

INITIALIZE_PASS(CXXCopyElisionLegacyPass, "cxx_copy_elision",
                "CXX Copy Elision", false, false)

FunctionPass *llvm::createCXXCopyElisionPass() {
  return new CXXCopyElisionLegacyPass();
}