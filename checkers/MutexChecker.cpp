// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/CheckerRegistry.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"

using namespace clang;
using namespace ento;

namespace std {
  void terminate( void ) _NOEXCEPT {
    abort();
  }
}

/* This analyzer suffers from the major limitation that most of the mutexes in Pebble are globals,
 * so all symbols and MemRegions referring to the mutexes are invalidated every time an unknown
 * function is called. This analyzer instead associates mutexes with the declaration of their
 * variables, which has the obvious limitation of not catching when mutexes are passed as
 * arguments (which fortunately never? happens in pebble).
 */

class MutexState {
  private:
    bool locked;
    bool recursive;
    unsigned lockCount;
  public:
    MutexState(bool isLocked, bool isRecursive, unsigned startCount)
      : 
        locked(isLocked),
        recursive(isRecursive),
        lockCount(startCount)
    {}

    MutexState getLocked() const {
      if (recursive) {
        if (locked) {
          // Preserve the first lock function (it should be the last one to unlock)
          return MutexState(true, true, lockCount + 1);
        }
        else {
          return MutexState(true, true, lockCount + 1);
        }
      }
      else {
        return MutexState(true, false, 0);
      }
    }

    MutexState getUnlocked(void) const {
      if (recursive) {
        // If lockCount is one, we unlock
        return MutexState(lockCount > 1, true, lockCount - 1);
      }
      else {
        return MutexState(false, false, 0);
      }
    }

    bool isLocked(void) const {
      return locked;
    }

    bool isRecursive(void) const {
      return recursive;
    }

    bool operator==(const MutexState &other) const {
      return locked == other.locked && recursive == other.recursive &&
        lockCount == other.lockCount;
    }

    void Profile(llvm::FoldingSetNodeID &ID) const {
      ID.AddBoolean(locked);
      ID.AddBoolean(recursive);
      ID.AddInteger(lockCount);
    }
};

// Map mutex declarations to state info
REGISTER_MAP_WITH_PROGRAMSTATE(MutexMap, const Decl *, MutexState);

// Hold an ordered list of the mutexes to catch lock order reversal
REGISTER_LIST_WITH_PROGRAMSTATE(MutexList, const Decl *);

namespace {
  class MutexChecker : public Checker<check::PostCall, check::EndFunction> {
    std::unique_ptr<BugType> NoUnlockBugType;
    std::unique_ptr<BugType> DoubleLockBugType;
    std::unique_ptr<BugType> DoubleUnlockBugType;
    std::unique_ptr<BugType> TooManyUnlocksBugType;
    std::unique_ptr<BugType> UnlockNoLockBugType;
    std::unique_ptr<BugType> LockReversalBugType;

    void reportError(const std::unique_ptr<BugType> &bugType, StringRef msg, CheckerContext &C) const {
      ExplodedNode *endNode = C.generateSink();
      if (!endNode) {
        return;
      }
      BugReport *bug = new BugReport(*bugType, msg, endNode);
      C.emitReport(bug);
    }

    ProgramStateRef lockMutex(const Decl *mutexDecl, const MutexState *curMutex, 
       ProgramStateRef state, bool recursive = false) const {

      state = state->add<MutexList>(mutexDecl);
      if (curMutex) {
        MutexState lockedMutex = curMutex->getLocked();
        return state->set<MutexMap>(mutexDecl, lockedMutex);
      }
      else {
        MutexState newMutex(true, recursive, recursive ? 1 : 0);
        return state->set<MutexMap>(mutexDecl, newMutex);
      }
    }

    const Decl * getMutexDecl(const Expr *argExpr) const {
        const Expr *strippedExpr = argExpr->IgnoreParenCasts();
        const DeclRefExpr *ref = dyn_cast<DeclRefExpr>(strippedExpr);
        if (ref) {
          return ref->getDecl();
        }
        // If it wasn't a DeclRef maybe it was a member?
        const MemberExpr *member = dyn_cast<MemberExpr>(strippedExpr);
        if (member) {
          return member->getMemberDecl();
        }

        return NULL;
    }

    void handleLock(StringRef funcName, const CallEvent &call, CheckerContext &C) const {
      const Decl *mutexDecl = getMutexDecl(call.getArgExpr(0));

      if (!mutexDecl) {
        return;
      }

      ProgramStateRef state = C.getState();

      const MutexState *curMutex = state->get<MutexMap>(mutexDecl);

      if (funcName.equals("mutex_lock") || funcName.equals("mutex_lock_with_lr")) {
        if (curMutex) {
          if (curMutex->isLocked()) {
            reportError(DoubleLockBugType, "This lock was already locked", C); 
            return;
          }
        }
        state = lockMutex(mutexDecl, curMutex, state);
        C.addTransition(state);
      }
      else if (funcName.equals("mutex_lock_with_timeout")) {
        if (curMutex) {
          if (curMutex->isLocked()) {
            reportError(DoubleLockBugType, "This lock was already locked", C); 
            return;
          }
        }
        // diverge into two states, one where we get the mutex and one
        // where we don't
        ProgramStateRef lockedState, timeoutState;

        DefinedSVal retVal = call.getReturnValue().castAs<DefinedSVal>();
        std::tie(lockedState, timeoutState) = state->assume(retVal);

        lockedState = lockMutex(mutexDecl, curMutex, lockedState);

        C.addTransition(lockedState);
        C.addTransition(timeoutState);
      }
      else if (funcName.equals("mutex_lock_recursive")) {
        state = lockMutex(mutexDecl, curMutex, state, true);
        C.addTransition(state);
      }
      else if (funcName.equals("mutex_lock_recursive_with_timeout") ||
               funcName.equals("mutex_lock_recursive_with_timeout_and_lr")) {
        ProgramStateRef lockedState, timeoutState;

        DefinedSVal retVal = call.getReturnValue().castAs<DefinedSVal>();
        std::tie(lockedState, timeoutState) = state->assume(retVal);

        lockedState = lockMutex(mutexDecl, curMutex, lockedState, true);

        C.addTransition(lockedState);
        C.addTransition(timeoutState);
      }
    }

    void handleUnlock(StringRef funcName, const CallEvent &call, CheckerContext &C) const {
      if (!(funcName.equals("mutex_unlock") || funcName.equals("mutex_unlock_recursive"))) {
        return;
      }
      ProgramStateRef state = C.getState();

      const Decl *mutexDecl = getMutexDecl(call.getArgExpr(0));
      const MutexState *curMutex = state->get<MutexMap>(mutexDecl);

      // If it isn't in the map, we never locked it
      if (!curMutex) {
        reportError(UnlockNoLockBugType, "Mutex was never locked", C);
        return;
      }
      // If it is in the map but unlocked, it was unlocked twice
      if (!curMutex->isLocked()) {
        if (curMutex->isRecursive()) {
          reportError(TooManyUnlocksBugType, "Recursive mutex already fully unlocked", C);
        }
        else {
          reportError(DoubleUnlockBugType, "Mutex already unlocked", C);
        }
        return;
      }

      const Decl *lastDecl = state->get<MutexList>().getHead();

      if (mutexDecl != lastDecl) {
        reportError(LockReversalBugType, "This was not the most recently acquired lock", C);
        return;
      }

      state = state->set<MutexList>(state->get<MutexList>().getTail());
      state = state->set<MutexMap>(mutexDecl, curMutex->getUnlocked());
      C.addTransition(state);
    }

    public:
    MutexChecker(void)
      : NoUnlockBugType(new BugType(this, "Failure to call unlock", "Pebble Mutex Plugin")),
        DoubleLockBugType(new BugType(this, "Double Lock", "Pebble Mutex Plugin")),
        DoubleUnlockBugType(new BugType(this, "Double Unlock", "Pebble Mutex Plugin")),
        TooManyUnlocksBugType(new BugType(this, "More unlocks than locks", "Pebble Mutex Plugin")),
        UnlockNoLockBugType(new BugType(this, "Unlock called before lock", "Pebble Mutex Plugin")),
        LockReversalBugType(new BugType(this, "Lock order reversal", "Pebble Mutex Plugin"))
    {}

    void checkPostCall(const CallEvent &call, CheckerContext &C) const {
      const IdentifierInfo *identInfo = call.getCalleeIdentifier();
      if(!identInfo) {
        return;
      }
      StringRef funcName = identInfo->getName();
      if (funcName.startswith("mutex_lock")) {
        handleLock(funcName, call, C);
      }
      else if (funcName.startswith("mutex_unlock")) {
        handleUnlock(funcName, call, C);
      }
    }

    void checkEndFunction(CheckerContext &C) const {
      ProgramStateRef state = C.getState();

      if (C.inTopFrame()) {
        // This path ends once this function ends
        for (auto mutexPair : state->get<MutexMap>()) { 
          if (mutexPair.second.isLocked()) {
            reportError(NoUnlockBugType, "Mutex still locked at end of path", C);
            return;
          }
        }
      }
    }
  };
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_ANALYZER_API_VERSION_STRING;

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<MutexChecker>("pebble.MutexChecker", "Checker for use of mutex_lock()/mutex_unlock()");
}
