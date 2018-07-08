#include "DSWP.hpp"
#include "SCCDAGPartition.hpp"

using namespace llvm;

static void partitionHeuristics (DSWPLoopDependenceInfo *LDI);

void DSWP::partitionSCCDAG (DSWPLoopDependenceInfo *LDI) {

  /*
   * Initial the partition structure with the merged SCCDAG
   */
  LDI->partitions.initialize(LDI->loopSCCDAG, &LDI->sccdagInfo, &LDI->liSummary, /*idealThreads=*/ 2);

  /*
   * Check if we can cluster SCCs.
   */
  if (!this->forceNoSCCPartition) {
    clusterSubloops(LDI);
  }

  /*
   * Assign SCCs that have no partition to their own partitions.
   */
  for (auto nodePair : LDI->loopSCCDAG->internalNodePairs()) {

    /*
     * Check if the current SCC can be removed (e.g., because it is due to induction variables).
     * If it is, then this SCC has already been assigned to every dependent partition.
     */
    auto currentSCC = nodePair.first;
    if (LDI->partitions.isRemovable(currentSCC)) continue ;

    /*
     * Check if the current SCC has been already assigned to a partition; if not, assign it to a new partition.
     */
    if (LDI->partitions.partitionOf(currentSCC) == nullptr) {
      LDI->partitions.addPartition(nodePair.first);
    }
  }

  /*
   * Print the initial partitions.
   */
  if (this->verbose >= Verbosity::Maximal) {
    errs() << "DSWP:  Before partitioning the SCCDAG\n";
    printPartitions(LDI);
  }

  /*
   * Check if we can cluster SCCs.
   */
  if (!this->forceNoSCCPartition) {

    /*
     * Decide the partition of the SCCDAG by merging the trivial partitions defined above.
     */
    partitionHeuristics(LDI);
  }

  /*
   * Print the partitioned SCCDAG.
   */
  if (this->verbose >= Verbosity::Maximal) {
    errs() << "DSWP:  After partitioning the SCCDAG\n";
    printPartitions(LDI);
  }

  return ;
}

void DSWP::mergeTrivialNodesInSCCDAG (DSWPLoopDependenceInfo *LDI) {

  /*
   * Print the current SCCDAG.
   */
  if (this->verbose >= Verbosity::Maximal) {
    errs() << "DSWP:  Before merging SCCs\n";
    printSCCs(LDI->loopSCCDAG);
  }

  /*
   * Merge SCCs.
   */
  mergePointerLoadInstructions(LDI);
  mergeSinglePHIs(LDI);
  mergeBranchesWithoutOutgoingEdges(LDI);

  /*
   * Print the current SCCDAG.
   */
  if (this->verbose >= Verbosity::Maximal) {
    errs() << "DSWP:  After merging SCCs\n";
    printSCCs(LDI->loopSCCDAG);
  }

  return ;
}

void DSWP::mergePointerLoadInstructions (DSWPLoopDependenceInfo *LDI) {
  while (true) {
    auto mergeNodes = false;
    for (auto sccEdge : LDI->loopSCCDAG->getEdges()) {
      auto fromSCCNode = sccEdge->getOutgoingNode();
      auto toSCCNode = sccEdge->getIncomingNode();
      for (auto instructionEdge : sccEdge->getSubEdges()) {
        auto producer = instructionEdge->getOutgoingT();
        bool isPointerLoad = isa<GetElementPtrInst>(producer);
        isPointerLoad |= (isa<LoadInst>(producer) && producer->getType()->isPointerTy());
        if (!isPointerLoad) continue;
        mergeNodes = true;
      }

      if (mergeNodes) {
        std::set<DGNode<SCC> *> GEPGroup = { fromSCCNode, toSCCNode };
        LDI->loopSCCDAG->mergeSCCs(GEPGroup);
        break;
      }
    }
    if (!mergeNodes) break;
  }
}

void DSWP::mergeSinglePHIs (DSWPLoopDependenceInfo *LDI) {
  std::vector<std::set<DGNode<SCC> *>> singlePHIs;
  for (auto sccNode : LDI->loopSCCDAG->getNodes()) {
    auto scc = sccNode->getT();
    if (scc->numInternalNodes() > 1) continue;
    if (!isa<PHINode>(scc->begin_internal_node_map()->first)) continue;
    if (sccNode->numOutgoingEdges() == 1) {
      std::set<DGNode<SCC> *> nodes = { sccNode, (*sccNode->begin_outgoing_edges())->getIncomingNode() };
      singlePHIs.push_back(nodes);
    }
  }

  for (auto sccNodes : singlePHIs) LDI->loopSCCDAG->mergeSCCs(sccNodes);
}

void DSWP::clusterSubloops (DSWPLoopDependenceInfo *LDI) {
  auto &li = LDI->liSummary;
  auto loop = li.bbToLoop[LDI->header];
  auto loopDepth = loop->depth;

  unordered_map<LoopSummary *, std::set<SCC *>> loopSets;
  for (auto sccNode : LDI->loopSCCDAG->getNodes()) {
    for (auto iNodePair : sccNode->getT()->internalNodePairs()) {
      auto bb = cast<Instruction>(iNodePair.first)->getParent();
      auto subL = li.bbToLoop[bb];
      auto subDepth = subL->depth;
      if (loopDepth >= subDepth) continue;

      while (subDepth - 1 > loopDepth) {
        subL = subL->parent;
        subDepth--;
      }
      loopSets[subL].insert(sccNode->getT());
      break;
    }
  }

  /*
   * Basic Heuristic: partition entire sub loops only if there is more than one
   */
  if (loopSets.size() == 1) return;
  for (auto loopSetPair : loopSets) {
    LDI->partitions.addPartition(loopSetPair.second);
  }
}

void DSWP::mergeBranchesWithoutOutgoingEdges (DSWPLoopDependenceInfo *LDI) {
  std::vector<DGNode<SCC> *> tailCmpBrs;
  for (auto sccNode : LDI->loopSCCDAG->getNodes()) {
    auto scc = sccNode->getT();
    if (sccNode->numIncomingEdges() == 0 || sccNode->numOutgoingEdges() > 0) continue ;

    bool allCmpOrBr = true;
    for (auto node : scc->getNodes()) {
      allCmpOrBr &= (isa<TerminatorInst>(node->getT()) || isa<CmpInst>(node->getT()));
    }
    if (allCmpOrBr) tailCmpBrs.push_back(sccNode);
  }

  /*
   * Merge trailing compare/branch scc into previous depth scc
   */
  for (auto tailSCC : tailCmpBrs) {
    std::set<DGNode<SCC> *> nodesToMerge = { tailSCC };
    nodesToMerge.insert(*LDI->loopSCCDAG->previousDepthNodes(tailSCC).begin());
    LDI->loopSCCDAG->mergeSCCs(nodesToMerge);
  }
}

void DSWP::addRemovableSCCsToStages (DSWPLoopDependenceInfo *LDI) {
  for (auto &stage : LDI->stages) {
    std::set<DGNode<SCC> *> visitedNodes;
    std::queue<DGNode<SCC> *> dependentSCCNodes;

    for (auto scc : stage->stageSCCs) {
      dependentSCCNodes.push(LDI->loopSCCDAG->fetchNode(scc));
    }

    while (!dependentSCCNodes.empty()) {
      auto depSCCNode = dependentSCCNodes.front();
      dependentSCCNodes.pop();

      for (auto sccEdge : depSCCNode->getIncomingEdges()) {
        auto fromSCCNode = sccEdge->getOutgoingNode();
        auto fromSCC = fromSCCNode->getT();
        if (visitedNodes.find(fromSCCNode) != visitedNodes.end()) continue;
        if (!LDI->partitions.isRemovable(fromSCC)) continue;

        stage->removableSCCs.insert(fromSCC);
        dependentSCCNodes.push(fromSCCNode);
        visitedNodes.insert(fromSCCNode);
      }
    }
  }
}

static void partitionHeuristics (DSWPLoopDependenceInfo *LDI) {

  /*
   * Collect all top level partitions, following (producer -> consumer) dependencies to pass over removable SCCs.
   */
  std::set<DGNode<SCC> *> topLevelSCCNodes = LDI->loopSCCDAG->getTopLevelNodes();
  // for (auto sccNode : topLevelSCCNodes) sccNode->getT()->print(errs() << "DSWP:   TOP LEVEL SCC\n", "DSWP:   ---") << "\n";
  std::set<SCCDAGPartition *> topLevelParts = LDI->partitions.getDependents(topLevelSCCNodes);
  std::queue<SCCDAGPartition *> partToCheck;
  for (auto part : topLevelParts) {
    if (LDI->partitions.getAncestors(part).size() == 0) {
      // part->print(errs() << "DSWP:   TOP LEVEL PARTITION:\n", "DSWP:   ");
      partToCheck.push(part);
    }
  }

  /*
   * Merge partitions.
   */
  std::set<SCCDAGPartition *> deletedPartitions;
  while (!partToCheck.empty()) {

    /*
     * Fetch the current partition.
     */
    auto partition = partToCheck.front();
    partToCheck.pop();

    // partition->print(errs() << "DSWP:   CHECKING PARTITION:\n", "DSWP:   ");

    /*
     * Check if the current partition has been already tagged to be removed (i.e., merged).
     */
    if (deletedPartitions.find(partition) != deletedPartitions.end()) {
      continue;
    }

    /*
     * Locate the best partition that the current one should merge with.
     */
    SCCDAGPartition *minPartition = nullptr;
    int32_t maxNumSquashedEdges = 0;
    auto maxCost = LDI->partitions.maxPartitionCost();
    auto dependents = LDI->partitions.getDependents(partition);
    for (auto depPartition : dependents) {
      if ((depPartition->cost + partition->cost) > maxCost) {
        continue;
      }

      auto squashedEdges = LDI->partitions.numEdgesBetween(partition, depPartition);
      if (squashedEdges > maxNumSquashedEdges) {
        minPartition = depPartition;
        maxNumSquashedEdges = squashedEdges;
      }
    }

    /*
     * Merge partition if one is found; iterate over all dependent partitions
     */
    for (auto depPartition : dependents) {
      if (minPartition == depPartition) {
        deletedPartitions.insert(partition);
        deletedPartitions.insert(depPartition);
        partToCheck.push(LDI->partitions.mergePartitions(partition, depPartition));
      } else {
        partToCheck.push(depPartition);
      }
    }
  }

  return ;
}
