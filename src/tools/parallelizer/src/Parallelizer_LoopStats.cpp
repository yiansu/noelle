#include "Parallelizer_LoopStats.hpp"

namespace llvm::noelle {

  Parallelizer_LoopStats::Parallelizer_LoopStats() : ModulePass{ID} {
    return ;
  }

  bool Parallelizer_LoopStats::doInitialization (Module &M) {
    return false;
  }

  bool Parallelizer_LoopStats::runOnModule (Module &M) {
    errs() << "Parallelizer_LoopStats: Start\n";
    std::string outputPrefix{ "Parallelizer_LoopStats:    " };

    auto &noelle = getAnalysis<Noelle>();
    auto heuristics = getAnalysis<HeuristicsPass>().getHeuristics(noelle);
    auto profiles = noelle.getProfiles();
    auto verbosity = noelle.getVerbosity();
    DOALL doall{ noelle };

    auto programLoops = noelle.getLoopStructures();
    if (programLoops->size() == 0) {
      errs() << outputPrefix << "There is no loop to consider\n";
      return false;
    }
    errs() << outputPrefix << "There are " << programLoops->size() << " loops in the program\n";
    
    /*
     * Compute the nesting forest
     */
    auto forest = noelle.organizeLoopsInTheirNestingForest(*programLoops);
    delete programLoops;
    auto trees = forest->getTrees();
    errs() << outputPrefix << "There are " << trees.size() << " loop nesting trees in the program\n";

    /*
     * Tree and coverage information
     */
    std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> loopTreeCoverage;

    /*
     * Nested loops and their coverage information
     */
    std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> nestedLoopAndCoverage;

    /*
     * Savings for every loop
     */
    std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> loopSavings;

    /*
     * Go through every loop nest tree and apply lambda function on each loop
     */
    for (auto tree : trees) {
      auto loopStructure = tree->getLoop();
      auto optimizations = { LoopDependenceInfoOptimization::MEMORY_CLONING_ID, LoopDependenceInfoOptimization::THREAD_SAFE_LIBRARY_ID };
      auto ldi = noelle.getLoop(loopStructure, optimizations);

      /*
       * Collect loop tree coverage information
       */
      loopTreeCoverage[tree] = profiles->getDynamicTotalInstructionCoverage(loopStructure) * 100;

      /*
       * Collect nested loop coverage information if outer loop is not DOALL
       */
      if (!tree->getDescendants().empty() && !doall.canBeAppliedToLoop(ldi, noelle, heuristics)) {
        nestedLoopAndCoverage[tree] = profiles->getDynamicTotalInstructionCoverage(loopStructure) * 100;
      }

      /*
       * Lambda function to print loop stats per tree node
       */
      auto printLoop = [&noelle, profiles, &outputPrefix, &optimizations, &doall, heuristics, &loopSavings](noelle::StayConnectedNestedLoopForestNode *n, uint32_t treeLevel) -> bool {

        /*
         * Fetch the loop information.
         */
        auto loopStructure = n->getLoop();
        auto loopID = loopStructure->getID();
        auto loopFunction = loopStructure->getFunction();
        auto loopHeader = loopStructure->getHeader();
        auto ldi = noelle.getLoop(loopStructure, optimizations);

        /*
         * Fetch the set of sequential SCCs and get the largest one
         */
        auto sequentialSCCs = DOALL::getSCCsThatBlockDOALLToBeApplicable(ldi, noelle);
        uint64_t biggestSCCTime = 0;
        for (auto sequentialSCC : sequentialSCCs) {
          auto sequentialSCCTime = profiles->getTotalInstructions(sequentialSCC);
          if (sequentialSCCTime > biggestSCCTime) {
            biggestSCCTime = sequentialSCCTime;
          }
        }
        auto instsPerIteration = profiles->getAverageTotalInstructionsPerIteration(loopStructure);
        auto instsInBiggestSCCPerIteration = ((double)biggestSCCTime / (double)profiles->getIterations(loopStructure));
        auto timeSavedPerIteration = (double)(instsPerIteration - instsInBiggestSCCPerIteration);
        auto timeSaved = timeSavedPerIteration * profiles->getIterations(loopStructure);
        loopSavings[n] = (uint64_t)timeSaved;

        /*
         * Compute the print prefix.
         */
        std::string prefix{"Parallelizer_LoopStats:    "};
        for (auto i = 1 ; i < treeLevel; i++){
          prefix.append("  ");
        }

        /*
         * Print the loop.
         */
        errs() << prefix << "ID: " << loopID << "\n";
        errs() << prefix << "  Function: \"" << loopFunction->getName() << "\"\n";
        errs() << prefix << "  Loop: \"" << *loopHeader->getFirstNonPHI() << "\"\n";
        errs() << prefix << "  Loop nesting level: " << loopStructure->getNestingLevel() << "\n";

        /*
         * Print the stats of this loop.
         */
        errs() << prefix << "  Hotness/Coverage = " << profiles->getDynamicTotalInstructionCoverage(loopStructure) * 100 << " %\n";
        errs() << prefix << "  DOALLable?: " << (doall.canBeAppliedToLoop(ldi, noelle, heuristics) ? "true" : "false") << "\n";
        errs() << prefix << "  Savings: " << loopSavings[n] << "\n";
        errs() << prefix << "  Average iterations per invocation = " << profiles->getAverageLoopIterationsPerInvocation(loopStructure) << "\n";
        errs() << prefix << "  Average instructions per invocation = " << profiles->getAverageTotalInstructionsPerInvocation(loopStructure) << "\n"; 
        errs() << prefix << "\n";

        return false;
      };

      tree->visitPreOrder(printLoop);
    }

    /*
     * DOALL coverage
     */
    std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> doallCoverage;

    /*
     * Collect DOALL coverage
     */
    for (auto tree : trees) {
      collectDOALLCoverage(tree, noelle, heuristics, profiles, doall, doallCoverage);
    }

    /*
     * Print total coverage information
     */
    double coverageForAllLoopTrees = 0.0;
    for (auto pair : loopTreeCoverage) {
      coverageForAllLoopTrees += pair.second;
    }
    errs() << outputPrefix << "Total loop tree coverage: " << coverageForAllLoopTrees << "\n";
    if (coverageForAllLoopTrees > 100.0) {
      errs() << outputPrefix << "Attention!! Total loop trees coverage sum over 100\n";
    }

    /*
     * Print and save nested loop coverage informtion
     */
    string nestedLoopCoverageList = createCoverageListString(nestedLoopAndCoverage);
    errs() << outputPrefix << "Nested loop coverage list: " << nestedLoopCoverageList << "\n";
    saveCoverageListString(nestedLoopCoverageList, "nested_loop_coverage_list.txt");

    double coverageForNestedLoops = 0.0;
    for (auto pair : nestedLoopAndCoverage) {
      coverageForNestedLoops += pair.second;
    }
    errs() << outputPrefix << "Totoal nested loop coverage: " << coverageForNestedLoops << "\n";
    if (coverageForNestedLoops > 100.0) {
      errs() << outputPrefix << "Attention!! Total nested loop coverage sum over 100\n";
    }

    /*
     * Print and save doall coverage information
     */
    string doallLoopCoverageList = createCoverageListString(doallCoverage);
    errs() << outputPrefix << "DOALL coverage list: " << doallLoopCoverageList << "\n";
    saveCoverageListString(doallLoopCoverageList, "doall_coverage_list.txt");

    double coverageForDOALL = 0.0;
    for (auto pair : doallCoverage) {
      coverageForDOALL += pair.second;
    }
    errs() << outputPrefix << "Total DOALL coverage: " << coverageForDOALL << "\n";

    errs() << "Parallelizer_LoopStats: End\n";
    return false;
  }

  void Parallelizer_LoopStats::getAnalysisUsage (AnalysisUsage &AU) const {
    AU.addRequired<Noelle>();
    AU.addRequired<HeuristicsPass>();

    return ;
  }
}

void Parallelizer_LoopStats::collectDOALLCoverage(noelle::StayConnectedNestedLoopForestNode *tree, noelle::Noelle &noelle, noelle::Heuristics *heuristics, noelle::Hot *profiles, noelle::DOALL &doall, std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> &doallCoverage) {
  auto loopStructure = tree->getLoop();
  auto optimizations = { LoopDependenceInfoOptimization::MEMORY_CLONING_ID, LoopDependenceInfoOptimization::THREAD_SAFE_LIBRARY_ID };
  auto ldi = noelle.getLoop(loopStructure, optimizations);

  if (doall.canBeAppliedToLoop(ldi, noelle, heuristics)) {
    doallCoverage[tree] = profiles->getDynamicTotalInstructionCoverage(loopStructure) * 100;
    return ;
  }

  for (auto descend : tree->getDescendants()) {
    collectDOALLCoverage(descend, noelle, heuristics, profiles, doall, doallCoverage);
  }

  return ;
}

std::string Parallelizer_LoopStats::createCoverageListString(std::unordered_map<llvm::noelle::StayConnectedNestedLoopForestNode *, double> &coverageMap) {
  std::string coverageListString = "[";
  for (auto pair : coverageMap) {
    coverageListString += std::to_string(pair.second) + ",";
  }
  coverageListString += "]";

  return coverageListString;
}

void Parallelizer_LoopStats::saveCoverageListString(std::string &coverageString, const char *fileName) {
  std::ofstream file(fileName);
  file << coverageString << std::endl;
}

// Next there is code to register your pass to "opt"
char llvm::noelle::Parallelizer_LoopStats::ID = 0;
static RegisterPass<Parallelizer_LoopStats> X("parallelizer-loopstats", "Automatic parallelization of sequential code");

// Next there is code to register your pass to "clang"
static Parallelizer_LoopStats * _PassMaker = NULL;
static RegisterStandardPasses _RegPass1(PassManagerBuilder::EP_OptimizerLast,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
    if(!_PassMaker){ PM.add(_PassMaker = new Parallelizer_LoopStats());}}); // ** for -Ox
static RegisterStandardPasses _RegPass2(PassManagerBuilder::EP_EnabledOnOptLevel0,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
    if(!_PassMaker){ PM.add(_PassMaker = new Parallelizer_LoopStats());}});// ** for -O0
