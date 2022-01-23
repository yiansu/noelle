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

    private:

      void collectDOALLCoverage(noelle::StayConnectedNestedLoopForestNode *tree, noelle::Noelle &noelle, noelle::Heuristics *heuristics, noelle::Hot *profiles, noelle::DOALL &doall, std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> &doallCoverage);
      std::string createCoverageListString(std::unordered_map<llvm::noelle::StayConnectedNestedLoopForestNode *, double> &coverageMap);
      void saveCoverageListString(std::string &coverageString, const char *fileName);
  };
  
}