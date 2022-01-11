#pragma once

#include "Parallelizer.hpp"

namespace llvm::noelle {

  struct Parallelizer_LoopStats : public ModulePass {
    public:

      Parallelizer_LoopStats() ;

      bool doInitialization (Module &M) override ;

      bool runOnModule (Module &M) override ;

      void getAnalysisUsage (AnalysisUsage &AU) const override ;

      static char ID;
  };
  
}