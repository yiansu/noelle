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

    /*
     * Allocate the parallelization techniques.
     */
    DSWP dswp{ M, *profiles, false, true, verbosity };
    DOALL doall{ noelle };
    HELIX helix{ M, *profiles, false, verbosity };


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
     * Collect nested loops and their coverage information
     */
    std::unordered_map<noelle::StayConnectedNestedLoopForestNode *, double> nestedLoopAndCoverage;

    /*
     * Print loop stats
     */
    for (auto tree : trees) {

      if (!tree->getDescendants().empty()) {
        nestedLoopAndCoverage[tree] = profiles->getDynamicTotalInstructionCoverage(tree->getLoop()) * 100;
        // nestedLoopAndCoverage[tree] = profiles->getSelfTotalInstructionCoverage(tree->getLoop()) * 100;
      }

      /*
       * Lambda function to print loop stats per tree node
       */
      auto printTree = [&noelle, profiles, &outputPrefix, &doall, &helix, &dswp, heuristics](noelle::StayConnectedNestedLoopForestNode *n, uint32_t treeLevel) {

        /*
         * Fetch the loop information.
         */
        auto loopStructure = n->getLoop();
        auto loopID = loopStructure->getID();
        auto loopFunction = loopStructure->getFunction();
        auto loopHeader = loopStructure->getHeader();
        auto optimizations = { LoopDependenceInfoOptimization::MEMORY_CLONING_ID, LoopDependenceInfoOptimization::THREAD_SAFE_LIBRARY_ID };
        auto ldi = noelle.getLoop(loopStructure, optimizations);

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
         * Check if there are profiles.
         */
        if (!profiles->isAvailable()){
          errs() << prefix << "  !!! Profiler information is unavailable for this loop\n";
          return false;
        }

        /*
         * Print the stats of this loop.
         */
        auto averageIterations = profiles->getAverageLoopIterationsPerInvocation(loopStructure);
        errs() << prefix << "  Average iterations per invocation = " << averageIterations << " %\n";
        auto averageInstsPerInvocation = profiles->getAverageTotalInstructionsPerInvocation(loopStructure);
        errs() << prefix << "  Average instructions per invocation = " << averageInstsPerInvocation << " %\n"; 
        auto hotness = profiles->getDynamicTotalInstructionCoverage(loopStructure) * 100;
        // auto hotness = profiles->getSelfTotalInstructionCoverage(loopStructure) * 100;
        errs() << prefix << "  Hotness/Coverage = " << hotness << " %\n";
        errs() << prefix << "  DOALLable?: " << (doall.canBeAppliedToLoop(ldi, noelle, heuristics) ? "true" : "false") << "\n";
        
        errs() << prefix << "\n";

        return false;
      };

      tree->visitPreOrder(printTree);
    }

    string coverageList = "[";
    for (auto pair : nestedLoopAndCoverage) {
      coverageList += std::to_string(pair.second) + ",";
    }
    coverageList += "]";
    errs() << outputPrefix << "List: " << coverageList << "\n";
    std::ofstream file("coverage_list.txt");
    file << coverageList << std::endl;

    double coverageForNestedLoops = 0.0;
    double coverageForNestedLoopsIfOuterIsDOALL = 0.0;
    for (auto pair : nestedLoopAndCoverage) {
      auto node = pair.first;
      auto loopStructure = node->getLoop();
      auto optimizations = { LoopDependenceInfoOptimization::MEMORY_CLONING_ID, LoopDependenceInfoOptimization::THREAD_SAFE_LIBRARY_ID };
      auto ldi = noelle.getLoop(loopStructure, optimizations);
      coverageForNestedLoops += pair.second;

      // if outer loop is doallable, calculate coverages sum of inner loop
      if (doall.canBeAppliedToLoop(ldi, noelle, heuristics)) {
        for (auto descend : node->getDescendants()) {
          coverageForNestedLoopsIfOuterIsDOALL += profiles->getDynamicTotalInstructionCoverage(descend->getLoop()) * 100;
          // coverageForNestedLoopsIfOuterIsDOALL += profiles->getSelfTotalInstructionCoverage(descend->getLoop()) * 100;
        }
      } else {
        coverageForNestedLoopsIfOuterIsDOALL += pair.second;
      }
      
      errs() << outputPrefix << "ID: " << loopStructure->getID() << *loopStructure->getHeader()->getFirstNonPHI() << " with hotness " << pair.second << "\n";
    }
    errs() << outputPrefix << "There are " << nestedLoopAndCoverage.size() << " nested loops with a total coverage of " << coverageForNestedLoops << ", coverage excluding doallable outer loop is " << coverageForNestedLoopsIfOuterIsDOALL << "\n";
    if (coverageForNestedLoops > 100.0) {
      errs() << outputPrefix << "Attention!! Coverage sum over 100\n";
    }


    errs() << "Parallelizer_LoopStats: End\n";
    return false;
  }

  void Parallelizer_LoopStats::getAnalysisUsage (AnalysisUsage &AU) const {
    AU.addRequired<Noelle>();
    AU.addRequired<HeuristicsPass>();

    return ;
  }
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
