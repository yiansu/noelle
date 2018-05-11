#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"

#include "llvm/IR/Mangler.h"
#include "llvm/IR/IRBuilder.h"

#include "LoopDependenceInfo.hpp"
#include "PipelineInfo.hpp"
#include "PDG.hpp"
#include "SCC.hpp"
#include "SCCDAG.hpp"
#include "PDGAnalysis.hpp"

#include <unordered_map>
#include <set>
#include <queue>
#include <deque>

using namespace llvm;

namespace llvm {

  struct DSWP : public ModulePass {
    public:
      static char ID;

      Function *stageDispatcher;
      Function *printReachedI;

      std::vector<Function *> queuePushes;
      std::vector<Function *> queuePops;
      std::vector<Type *> queueTypes;
      std::vector<Type *> queueElementTypes;
      unordered_map<int, int> queueSizeToIndex;

      FunctionType *stageType;
      IntegerType *int1, *int8, *int16, *int32, *int64;

      DSWP() : ModulePass{ID} {}

      bool doInitialization (Module &M) override { return false; }

      bool runOnModule (Module &M) override
      {
        errs() << "DSWP for " << M.getName() << "\n";
        if (!collectThreadPoolHelperFunctionsAndTypes(M))
        {
          errs() << "DSWP utils not included!\n";
          return false;
        }

        auto graph = getAnalysis<PDGAnalysis>().getPDG();

        /*
         * Collect functions through call graph starting at function "main"
         */
        std::set<Function *> funcToModify;
        collectAllFunctionsInCallGraph(M, funcToModify);

        auto modified = false;
        for (auto F : funcToModify)
        {
          auto loopDI = fetchLoopToParallelize(*F, graph);
          if (loopDI == nullptr) {
            continue ;
          }

          /*
           * Parallelize the current loop with DSWP.
           */
          modified |= applyDSWP(loopDI);
          delete loopDI;
        }
        return modified;
      }

      void getAnalysisUsage (AnalysisUsage &AU) const override
      {
        AU.addRequired<PDGAnalysis>();
        AU.addRequired<AssumptionCacheTracker>();
        AU.addRequired<DominatorTreeWrapperPass>();
        AU.addRequired<PostDominatorTreeWrapperPass>();
        AU.addRequired<LoopInfoWrapperPass>();
        AU.addRequired<ScalarEvolutionWrapperPass>();
        AU.addRequired<CallGraphWrapperPass>();
        return ;
      }

    private:
      void collectAllFunctionsInCallGraph (Module &M, std::set<Function *> &funcSet)
      {
        auto &callGraph = getAnalysis<CallGraphWrapperPass>().getCallGraph();
        std::queue<Function *> funcToTraverse;
        funcToTraverse.push(M.getFunction("main"));
        while (!funcToTraverse.empty())
        {
          auto func = funcToTraverse.front();
          funcToTraverse.pop();
          if (funcSet.find(func) != funcSet.end()) continue;
          funcSet.insert(func);

          auto funcCGNode = callGraph[func];
          for (auto &callRecord : make_range(funcCGNode->begin(), funcCGNode->end()))
          {
            auto F = callRecord.second->getFunction();
            if (F->empty()) continue;
            funcToTraverse.push(F);
          }
        }
      }

      bool collectThreadPoolHelperFunctionsAndTypes (Module &M)
      {
        int1 = IntegerType::get(M.getContext(), 1);
        int8 = IntegerType::get(M.getContext(), 8);
        int16 = IntegerType::get(M.getContext(), 16);
        int32 = IntegerType::get(M.getContext(), 32);
        int64 = IntegerType::get(M.getContext(), 64);

        printReachedI = M.getFunction("printReachedI");
        std::string pushers[4] = { "queuePush8", "queuePush16", "queuePush32", "queuePush64" };
        std::string poppers[4] = { "queuePop8", "queuePop16", "queuePop32", "queuePop64" };
        for (auto pusher : pushers) queuePushes.push_back(M.getFunction(pusher));
        for (auto popper : poppers) queuePops.push_back(M.getFunction(popper));
        for (auto queueF : queuePushes) queueTypes.push_back(queueF->arg_begin()->getType());
        queueSizeToIndex = unordered_map<int, int>({ { 1, 0 }, { 8, 0 }, { 16, 1 }, { 32, 2 }, { 64, 3 }});
        queueElementTypes = std::vector<Type *>({ int8, int16, int32, int64 });

        stageDispatcher = M.getFunction("stageDispatcher");
        auto stageExecuter = M.getFunction("stageExecuter");

        auto stageArgType = stageExecuter->arg_begin()->getType();
        stageType = cast<FunctionType>(cast<PointerType>(stageArgType)->getElementType());
        return true;
      }

      LoopDependenceInfo *fetchLoopToParallelize (Function &function, PDG *graph)
      {
        /*
         * Fetch the loops.
         */
        auto &LI = getAnalysis<LoopInfoWrapperPass>(function).getLoopInfo();
        auto &DT = getAnalysis<DominatorTreeWrapperPass>(function).getDomTree();
        auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>(function).getPostDomTree();
        auto &SE = getAnalysis<ScalarEvolutionWrapperPass>(function).getSE();

        /*
         * Fetch the PDG.
         */
        auto funcPDG = graph->createFunctionSubgraph(function);

        /*
         * ASSUMPTION: One outermost loop for the function.
         *
         * We have to have one single outermost loop.
         */
        // if (std::distance(LI.begin(), LI.end()) != 1) return nullptr;

        /*
         * Choose the loop to parallelize.
         */
        for (auto loopIter : LI)
        {
          auto loop = &*loopIter;
          return new LoopDependenceInfo(&function, funcPDG, loop, LI, DT, PDT, SE);
        }

        return nullptr;
      }

      bool applyDSWP (LoopDependenceInfo *LDI)
      {
        errs() << "Applying DSWP\n";

        /*
         * Merge SCCs of the SCCDAG.
         */
        // printSCCs(LDI->loopSCCDAG);
        mergeSCCs(LDI);
        // printSCCs(LDI->loopSCCDAG);

        /*
         * Create the pipeline stages.
         */
        if (!isWorthParallelizing(LDI)) return false;
        if (!collectStageAndQueueInfo(LDI)) return false;
        // printStageSCCs(LDI);
        // printStageQueues(LDI);
        
        for (auto &stage : LDI->stages) createPipelineStageFromSCC(LDI, stage);

        /*
         * Create the pipeline (connecting the stages)
         */
        createPipelineFromStages(LDI);
        if (LDI->pipelineBB == nullptr)
        {
          for (auto &stage : LDI->stages) stage->sccStage->eraseFromParent();
          return false;
        }

        /*
         * Link the parallelized loop within the original function that includes the sequential loop.
         */
        linkParallelizedLoopToOriginalFunction(LDI);
        LDI->function->print(errs() << "Final printout:\n"); errs() << "\n";

        return true;
      }

      void mergeBranchesWithoutOutgoingEdges (LoopDependenceInfo *LDI)
      {
        auto &sccSubgraph = LDI->loopSCCDAG;
        std::vector<DGNode<SCC> *> tailBranches;
        for (auto sccNode : make_range(sccSubgraph->begin_nodes(), sccSubgraph->end_nodes()))
        {
          auto scc = sccNode->getT();
          if (scc->numInternalNodes() > 1) continue ;
          if (sccNode->numIncomingEdges() == 0) continue ;
          if (sccNode->numOutgoingEdges() > 0) continue ;
          
          auto singleInstrNode = *scc->begin_nodes();
          if (auto branch = dyn_cast<TerminatorInst>(singleInstrNode->getT())) tailBranches.push_back(sccNode);
        }

        /*
         * Merge trailing branch nodes into previous depth scc
         */
        for (auto tailBranch : tailBranches)
        {
          std::set<DGNode<SCC> *> nodesToMerge = { tailBranch };
          nodesToMerge.insert(*sccSubgraph->previousDepthNodes(tailBranch).begin());
          sccSubgraph->mergeSCCs(nodesToMerge);
        }
      }

      void mergeSCCs (LoopDependenceInfo *LDI)
      {
        errs() << "Number of unmerged nodes: " << LDI->loopSCCDAG->numNodes() << "\n";

        /*
         * Merge the SCC related to a single PHI node and its use if there is only one.
         */
        //TODO

        mergeBranchesWithoutOutgoingEdges(LDI);

        errs() << "Number of merged nodes: " << LDI->loopSCCDAG->numNodes() << "\n";
        return ;
      }

      bool isWorthParallelizing (LoopDependenceInfo *LDI)
      {
        return LDI->loopSCCDAG->numNodes() > 1;
      }

      void collectSCCIntoStages (LoopDependenceInfo *LDI)
      {
        auto topLevelSCCNodes = LDI->loopSCCDAG->getTopLevelNodes();

        /*
         * TODO: Check if all entries to the loop are into top level nodes
         */
        std::set<DGNode<SCC> *> nodesFound(topLevelSCCNodes.begin(), topLevelSCCNodes.end());
        std::deque<DGNode<SCC> *> nodesToTraverse(topLevelSCCNodes.begin(), topLevelSCCNodes.end());

        int order = 0;
        while (!nodesToTraverse.empty())
        {
          auto sccNode = nodesToTraverse.front();
          nodesToTraverse.pop_front();
          nodesFound.insert(sccNode);

          /*
           * Add all unvisited, next depth nodes to the traversal queue 
           */
          auto nextNodes = LDI->loopSCCDAG->nextDepthNodes(sccNode);
          for (auto next : nextNodes)
          {
            if (nodesFound.find(next) != nodesFound.end()) continue;
            nodesToTraverse.push_back(next);
          }

          auto scc = sccNode->getT();
          auto stage = std::make_unique<StageInfo>();
          stage->order = order++;
          stage->scc = scc;
          LDI->stages.push_back(std::move(stage));
          LDI->sccToStage[scc] = LDI->stages[order - 1].get();
        }
      }

      bool collectValueQueueInfo (LoopDependenceInfo *LDI)
      {
        std::map<Instruction *, StageInfo *> branchStageMap;
        for (auto scc : LDI->loopSCCDAG->getNodes())
        {
          for (auto sccEdge : scc->getOutgoingEdges())
          {
            auto sccPair = sccEdge->getNodePair();
            auto fromStage = LDI->sccToStage[sccPair.first->getT()];
            auto toStage = LDI->sccToStage[sccPair.second->getT()];
            if (fromStage == toStage) continue;

            /*
             * Create value and control queues for each dependency of the form: producer -> consumers
             */
            for (auto instructionEdge : sccEdge->getSubEdges())
            {
              assert(!instructionEdge->isMemoryDependence());

              auto pcPair = instructionEdge->getNodePair();
              auto producer = cast<Instruction>(pcPair.first->getT());
              auto consumer = cast<Instruction>(pcPair.second->getT());

              if (instructionEdge->isControlDependence())
              {
                branchStageMap[producer] = fromStage;
                continue;
              }
              
              int queueIndex = LDI->queues.size();
              for (auto queueI : fromStage->producerToQueues[producer])
              {
                if (LDI->queues[queueI]->toStage != toStage->order) continue;
                queueIndex = queueI;
                break;
              }

              if (queueIndex == LDI->queues.size())
              {
                LDI->queues.push_back(std::move(std::make_unique<QueueInfo>(producer, consumer, producer->getType())));
                fromStage->producerToQueues[producer].insert(queueIndex);
              }

              toStage->consumerToQueues[consumer].insert(queueIndex);
              fromStage->pushValueQueues.insert(queueIndex);
              toStage->popValueQueues.insert(queueIndex);

              auto queueInfo = LDI->queues[queueIndex].get();
              queueInfo->consumers.insert(consumer);
              queueInfo->fromStage = fromStage->order;
              queueInfo->toStage = toStage->order;

              if (queueSizeToIndex.find(queueInfo->bitLength) == queueSizeToIndex.end()) return false;
            }
          }
        }

        for (auto brStage : branchStageMap)
        {
          auto consumer = brStage.first;
          auto stage = brStage.second;
          auto brNode = stage->scc->fetchNode(consumer);
          for (auto edge : brNode->getIncomingEdges())
          {
            if (edge->isControlDependence()) continue;
            auto producer = cast<Instruction>(edge->getOutgoingT());
            for (auto &otherStage : LDI->stages)
            {
              if (otherStage.get() == stage) continue;
              int queueIndex = LDI->queues.size();
              LDI->queues.push_back(std::move(std::make_unique<QueueInfo>(producer, consumer, producer->getType())));
              stage->producerToQueues[producer].insert(queueIndex);
              otherStage->consumerToQueues[consumer].insert(queueIndex);
              stage->pushValueQueues.insert(queueIndex);
              otherStage->popValueQueues.insert(queueIndex);

              auto queueInfo = LDI->queues[queueIndex].get();
              queueInfo->consumers.insert(consumer);
              queueInfo->fromStage = stage->order;
              queueInfo->toStage = otherStage->order;
            }
          }
        }
        return true;
      }

      void collectEnvInfo (LoopDependenceInfo *LDI)
      {
        LDI->environment = std::make_unique<EnvInfo>();
        auto &externalDeps = LDI->environment->externalDependents;
        for (auto nodeI : LDI->loopDG->externalNodePairs())
        {
          auto externalNode = nodeI.second;
          auto externalValue = externalNode->getT();
          auto envIndex = externalDeps.size();
          externalDeps.push_back(externalValue);

          auto addExternalDependentToStagesWithInst = [&](Instruction *internalInst, bool outgoing) -> void {
            for (auto &stage : LDI->stages)
            {
              if (!stage->scc->isInternal(cast<Value>(internalInst))) continue;
              auto &envMap = outgoing ? stage->outgoingToEnvMap : stage->incomingToEnvMap;
              envMap[internalInst] = envIndex;
            }
            auto &envSet = outgoing ? LDI->environment->postLoopExternals : LDI->environment->preLoopExternals;
            envSet.insert(envIndex);
          };

          /*
           * Check if loop-external instruction has incoming/outgoing nodes within one of the stages
           */
          for (auto incomingEdge : externalNode->getIncomingEdges())
          {
            addExternalDependentToStagesWithInst(cast<Instruction>(incomingEdge->getOutgoingT()), true);
          }
          for (auto outgoingEdge : externalNode->getOutgoingEdges())
          {
            addExternalDependentToStagesWithInst(cast<Instruction>(outgoingEdge->getIncomingT()), false);
          }
        }
      }

      void configureDependencyStorage (LoopDependenceInfo *LDI)
      {
        LDI->zeroIndexForBaseArray = cast<Value>(ConstantInt::get(int64, 0));
        LDI->envArrayType = ArrayType::get(PointerType::getUnqual(int8), LDI->environment->envSize());
        LDI->queueArrayType = ArrayType::get(PointerType::getUnqual(int8), LDI->queues.size());
        LDI->stageArrayType = ArrayType::get(PointerType::getUnqual(int8), LDI->stages.size());
      }

      bool collectStageAndQueueInfo (LoopDependenceInfo *LDI)
      {
        collectSCCIntoStages(LDI);
        if (!collectValueQueueInfo(LDI)) return false;
        collectEnvInfo(LDI);
        configureDependencyStorage(LDI);
        return true;
      }

      void createInstAndBBForSCC (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        auto &context = LDI->function->getParent()->getContext();

        /*
         * Clone instructions within the stage's scc, and their basic blocks
         */
        for (auto nodePair : stageInfo->scc->internalNodePairs())
        {
          auto I = cast<Instruction>(nodePair.first);
          stageInfo->iCloneMap[I] = I->clone();
        }

        for (auto B : LDI->loop->blocks())
        {
          stageInfo->sccBBCloneMap[B] = BasicBlock::Create(context, "", stageInfo->sccStage);
          auto terminator = cast<Instruction>(B->getTerminator());
          if (stageInfo->iCloneMap.find(terminator) == stageInfo->iCloneMap.end())
          {
            stageInfo->iCloneMap[terminator] = terminator->clone();
          }
        }
        for (int i = 0; i < LDI->loopExitBlocks.size(); ++i)
        {
          stageInfo->sccBBCloneMap[LDI->loopExitBlocks[i]] = stageInfo->loopExitBlocks[i];
        }

        /*
         * Attach SCC instructions to their basic blocks in correct relative order
         */
        for (auto B : LDI->loop->blocks())
        {
          IRBuilder<> builder(stageInfo->sccBBCloneMap[B]);
          for (auto &I : *B)
          {
            if (stageInfo->iCloneMap.find(&I) == stageInfo->iCloneMap.end()) continue;
            auto iClone = stageInfo->iCloneMap[&I];
            builder.Insert(iClone);
          }
        }
      }

      void linkEnvironmentDependencies (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        IRBuilder<> entryBuilder(stageInfo->entryBlock);
        IRBuilder<> exitBuilder(stageInfo->exitBlock);
        auto envArg = &*(stageInfo->sccStage->arg_begin());
        auto envAlloca = entryBuilder.CreateBitCast(envArg, PointerType::getUnqual(LDI->envArrayType));

        auto accessEnvVarFromIndex = [&](int envIndex, IRBuilder<> builder) -> Value * {
          auto envIndexValue = cast<Value>(ConstantInt::get(int64, envIndex));
          auto envPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndexValue }));
          auto envType = LDI->environment->externalDependents[envIndex]->getType();
          return builder.CreateBitCast(builder.CreateLoad(envPtr), PointerType::getUnqual(envType));
        };

        /*
         * Store (SCC -> outside of loop) dependencies within the environment array
         */
        for (auto outgoingEnvPair : stageInfo->outgoingToEnvMap)
        {
          auto envVar = accessEnvVarFromIndex(outgoingEnvPair.second, exitBuilder);
          auto outgoingDepClone = stageInfo->iCloneMap[outgoingEnvPair.first];
          exitBuilder.CreateStore(outgoingDepClone, envVar);
        }

        /*
         * Store exit index in the exit environment variable
         */
        for (int i = 0; i < stageInfo->loopExitBlocks.size(); ++i)
        {
          IRBuilder<> builder(stageInfo->loopExitBlocks[i]);
          auto envIndexValue = cast<Value>(ConstantInt::get(int64, LDI->environment->externalDependents.size()));
          auto envPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndexValue }));
          auto envVar = builder.CreateBitCast(builder.CreateLoad(envPtr), PointerType::getUnqual(int32));
          builder.CreateStore(ConstantInt::get(int32, i), envVar);
        }

        /*
         * Load (outside of loop -> SCC) dependencies from the environment array 
         */
        for (auto incomingEnvPair : stageInfo->incomingToEnvMap)
        {
          auto envVar = accessEnvVarFromIndex(incomingEnvPair.second, entryBuilder);
          auto envLoad = entryBuilder.CreateLoad(envVar);

          Value *incomingDepValue = cast<Value>(incomingEnvPair.first);
          auto incomingDepClone = stageInfo->iCloneMap[incomingEnvPair.first];
          for (auto &depOp : incomingDepClone->operands())
          {
            if (depOp != incomingDepValue) continue;
            depOp.set(envLoad);
          }
        }
      }

      void remapLocalAndEnvOperandsOfInstClones (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        linkEnvironmentDependencies(LDI, stageInfo);

        /*
         * IMPROVEMENT: Ignore special cases upfront. If a clone of a general case is not found, abort with a corresponding error 
         */
        auto &iCloneMap = stageInfo->iCloneMap;
        for (auto ii = iCloneMap.begin(); ii != iCloneMap.end(); ++ii) {
          auto cloneInstruction = ii->second;

          for (auto &op : cloneInstruction->operands()) {
            auto opV = op.get();
            if (auto opI = dyn_cast<Instruction>(opV)) {
              auto iCloneIter = iCloneMap.find(opI);
              if (iCloneIter != iCloneMap.end()) {
                op.set(iCloneMap[opI]);
              }
              continue;
            }
            // Add cases such as constants where no clone needs to exist. Abort with an error if no such type is found
          }
        }
      }

      void loadAllQueuePointersInEntry (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        IRBuilder<> entryBuilder(stageInfo->entryBlock);
        auto argIter = stageInfo->sccStage->arg_begin();
        auto queuesArray = entryBuilder.CreateBitCast(&*(++argIter), PointerType::getUnqual(LDI->queueArrayType));

        /*
         * Load this stage's relevant queues
         */
        auto loadQueuePtrFromIndex = [&](int queueIndex) -> void {
          auto queueInfo = LDI->queues[queueIndex].get();
          auto queueIndexValue = cast<Value>(ConstantInt::get(int64, queueIndex));
          auto queuePtr = entryBuilder.CreateInBoundsGEP(queuesArray, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, queueIndexValue }));
          auto queueCast = entryBuilder.CreateBitCast(queuePtr, PointerType::getUnqual(queueTypes[queueSizeToIndex[queueInfo->bitLength]]));

          auto queueInstrs = std::make_unique<QueueInstrs>();
          queueInstrs->queuePtr = entryBuilder.CreateLoad(queueCast);
          queueInstrs->alloca = entryBuilder.CreateAlloca(queueInfo->dependentType);
          queueInstrs->allocaCast = entryBuilder.CreateBitCast(queueInstrs->alloca, PointerType::getUnqual(queueElementTypes[queueSizeToIndex[queueInfo->bitLength]]));
          stageInfo->queueInstrMap[queueIndex] = std::move(queueInstrs);
        };

        for (auto queueIndex : stageInfo->pushValueQueues) loadQueuePtrFromIndex(queueIndex);
        for (auto queueIndex : stageInfo->popValueQueues) loadQueuePtrFromIndex(queueIndex);
      }

      void popValueQueues (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        for (auto queueIndex : stageInfo->popValueQueues)
        {
          auto &queueInfo = LDI->queues[queueIndex];
          auto queueInstrs = stageInfo->queueInstrMap[queueIndex].get();
          auto queueCallArgs = ArrayRef<Value*>({ queueInstrs->queuePtr, queueInstrs->allocaCast });

          auto bb = queueInfo->producer->getParent();
          IRBuilder<> builder(stageInfo->sccBBCloneMap[bb]);
          queueInstrs->queueCall = builder.CreateCall(queuePops[queueSizeToIndex[queueInfo->bitLength]], queueCallArgs);
          queueInstrs->load = builder.CreateLoad(queueInstrs->alloca);

          /*
           * Position queue call and load relatively identically to where the producer is in the basic block
           */
          bool pastProducer = false;
          for (auto &I : *bb)
          {
            if (&I == queueInfo->producer) pastProducer = true;
            else if (pastProducer && stageInfo->iCloneMap.find(&I) != stageInfo->iCloneMap.end())
            {
              cast<Instruction>(queueInstrs->queueCall)->moveBefore(stageInfo->iCloneMap[&I]);
              cast<Instruction>(queueInstrs->load)->moveBefore(stageInfo->iCloneMap[&I]);
              break;
            }
          }
        }
      }

      void pushValueQueues (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        for (auto queueIndex : stageInfo->pushValueQueues)
        {
          auto queueInstrs = stageInfo->queueInstrMap[queueIndex].get();
          auto queueInfo = LDI->queues[queueIndex].get();
          auto queueCallArgs = ArrayRef<Value*>({ queueInstrs->queuePtr, queueInstrs->allocaCast });
          
          auto pClone = stageInfo->iCloneMap[queueInfo->producer];
          auto pCloneBB = pClone->getParent();
          IRBuilder<> builder(pCloneBB);
          auto store = builder.CreateStore(pClone, queueInstrs->alloca);
          queueInstrs->queueCall = builder.CreateCall(queuePushes[queueSizeToIndex[queueInfo->bitLength]], queueCallArgs);

          bool pastProducer = false;
          for (auto &I : *pCloneBB)
          {
            if (&I == pClone) pastProducer = true;
            else if (pastProducer)
            {
              store->moveBefore(&I);
              cast<Instruction>(queueInstrs->queueCall)->moveBefore(&I);
              
              if (pClone->getType() == int32)
              {
                //auto printCall = builder.CreateCall(printReachedI, ArrayRef<Value*>({ cast<Value>(pClone) }));
                //printCall->moveBefore(&I);
              }
              
              break;
            }
          }
        }
      }

      void remapValueConsumerOperands (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        for (auto queueIndex : stageInfo->popValueQueues)
        {
          auto queueInfo = LDI->queues[queueIndex].get();
          auto producer = cast<Value>(queueInfo->producer);
          auto load = stageInfo->queueInstrMap[queueIndex]->load;
          for (auto consumer : queueInfo->consumers)
          {
            for (auto &op : stageInfo->iCloneMap[consumer]->operands())
            {
              auto opV = op.get();
              if (opV != producer) continue;
              op.set(load);
            }
          }
        }
      }

      void remapControlFlow (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        auto &context = LDI->function->getContext();
        auto stageF = stageInfo->sccStage;

        for (auto bbPair : stageInfo->sccBBCloneMap)
        {
          auto originalT = bbPair.first->getTerminator();
          if (stageInfo->iCloneMap.find(originalT) == stageInfo->iCloneMap.end()) continue;
          auto terminator = cast<TerminatorInst>(stageInfo->iCloneMap[originalT]);
          for (int i = 0; i < terminator->getNumSuccessors(); ++i)
          {
            terminator->setSuccessor(i, stageInfo->sccBBCloneMap[terminator->getSuccessor(i)]);
          }
        }

        for (auto bbPair : stageInfo->sccBBCloneMap)
        {
          auto iIter = bbPair.second->begin();
          while (auto phi = dyn_cast<PHINode>(&*iIter))
          {
            for (auto bb : phi->blocks())
            {
              phi->setIncomingBlock(phi->getBasicBlockIndex(bb), stageInfo->sccBBCloneMap[bb]);
            }
            ++iIter;
          }
        }
      }

      void createPipelineStageFromSCC (LoopDependenceInfo *LDI, std::unique_ptr<StageInfo> &stageInfo)
      {
        auto M = LDI->function->getParent();
        auto stageF = cast<Function>(M->getOrInsertFunction("", stageType));
        auto &context = M->getContext();
        stageInfo->sccStage = stageF;
        stageInfo->entryBlock = BasicBlock::Create(context, "", stageF);
        stageInfo->exitBlock = BasicBlock::Create(context, "", stageF);
        stageInfo->sccBBCloneMap[LDI->loop->getLoopPreheader()] = stageInfo->entryBlock;
        for (auto exitBB : LDI->loopExitBlocks) stageInfo->loopExitBlocks.push_back(BasicBlock::Create(context, "", stageF));

        /*
         * SCC iteration
         */
        createInstAndBBForSCC(LDI, stageInfo);
        remapLocalAndEnvOperandsOfInstClones(LDI, stageInfo);

        loadAllQueuePointersInEntry(LDI, stageInfo);
        popValueQueues(LDI, stageInfo);
        remapValueConsumerOperands(LDI, stageInfo);
        pushValueQueues(LDI, stageInfo);
        remapControlFlow(LDI, stageInfo);

        IRBuilder<> entryBuilder(stageInfo->entryBlock);
        entryBuilder.CreateBr(stageInfo->sccBBCloneMap[LDI->loop->getHeader()]);

        /*
         * Cleanup
         */
        for (auto exitBB : stageInfo->loopExitBlocks)
        {
          IRBuilder<> builder(exitBB);
          builder.CreateBr(stageInfo->exitBlock);
        }
        IRBuilder<> exitBuilder(stageInfo->exitBlock);
        exitBuilder.CreateRetVoid();
        stageF->print(errs() << "Function printout:\n"); errs() << "\n";
      }

      Value * createEnvArrayFromStages (LoopDependenceInfo *LDI, IRBuilder<> builder, Value *envAlloca)
      {
        /*
         * Create empty environment array with slots for external values dependent on loop values
         */
        std::vector<Value*> envPtrsForDep;
        auto extDepSize = LDI->environment->externalDependents.size();
        for (int i = 0; i < extDepSize; ++i)
        {
          Type *envType = LDI->environment->externalDependents[i]->getType();
          auto envVarPtr = builder.CreateAlloca(envType);
          envPtrsForDep.push_back(envVarPtr);
          auto envIndex = cast<Value>(ConstantInt::get(int64, i));
          auto depInEnvPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndex }));

          auto depCast = builder.CreateBitCast(depInEnvPtr, PointerType::getUnqual(PointerType::getUnqual(envType)));
          builder.CreateStore(envVarPtr, depCast);
        }

        /*
         * Add exit block tracking variable to env
         */
        auto exitVarPtr = builder.CreateAlloca(int32);
        auto envIndex = cast<Value>(ConstantInt::get(int64, extDepSize));
        auto varInEnvPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndex }));
        auto depCast = builder.CreateBitCast(varInEnvPtr, PointerType::getUnqual(PointerType::getUnqual(int32)));
        builder.CreateStore(exitVarPtr, depCast);

        /*
         * Insert incoming dependents for stages into the environment array
         */
        for (int envIndex : LDI->environment->preLoopExternals)
        {
          builder.CreateStore(LDI->environment->externalDependents[envIndex], envPtrsForDep[envIndex]);
        }
        
        return cast<Value>(builder.CreateBitCast(envAlloca, PointerType::getUnqual(int8)));
      }

      Value * createQueueSizesArrayFromStages (LoopDependenceInfo *LDI, IRBuilder<> builder)
      {
        auto queuesAlloca = cast<Value>(builder.CreateAlloca(ArrayType::get(int64, LDI->queues.size())));
        for (int i = 0; i < LDI->queues.size(); ++i)
        {
          auto &queue = LDI->queues[i];
          auto queueIndex = cast<Value>(ConstantInt::get(int64, i));
          auto queuePtr = builder.CreateInBoundsGEP(queuesAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, queueIndex }));
          auto queueCast = builder.CreateBitCast(queuePtr, PointerType::getUnqual(int64));
          builder.CreateStore(ConstantInt::get(int64, queue->bitLength), queueCast);
        }
        return cast<Value>(builder.CreateBitCast(queuesAlloca, PointerType::getUnqual(int64)));
      }

      Value * createStagesArrayFromStages (LoopDependenceInfo *LDI, IRBuilder<> builder)
      {
        auto stagesAlloca = cast<Value>(builder.CreateAlloca(LDI->stageArrayType));
        auto stageCastType = PointerType::getUnqual(LDI->stages[0]->sccStage->getType());
        for (int i = 0; i < LDI->stages.size(); ++i)
        {
          auto &stage = LDI->stages[i];
          auto stageIndex = cast<Value>(ConstantInt::get(int64, i));
          auto stagePtr = builder.CreateInBoundsGEP(stagesAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, stageIndex }));
          auto stageCast = builder.CreateBitCast(stagePtr, stageCastType);
          builder.CreateStore(stage->sccStage, stageCast);
        }
        return cast<Value>(builder.CreateBitCast(stagesAlloca, PointerType::getUnqual(int8)));
      }

      void storeOutgoingDependentsIntoExternalValues (LoopDependenceInfo *LDI, IRBuilder<> builder, Value *envAlloca)
      {
        /*
         * Extract the outgoing dependents for each stage
         */
        for (int envInd : LDI->environment->postLoopExternals)
        {
          auto depI = LDI->environment->externalDependents[envInd];
          auto envIndex = cast<Value>(ConstantInt::get(int64, envInd));
          auto depInEnvPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndex }));
          auto envVarCast = builder.CreateBitCast(builder.CreateLoad(depInEnvPtr), PointerType::getUnqual(depI->getType()));
          auto envVar = builder.CreateLoad(envVarCast);

          if (auto depPHI = dyn_cast<PHINode>(depI))
          {
            depPHI->addIncoming(envVar, LDI->pipelineBB);
            continue;
          }
          LDI->pipelineBB->eraseFromParent();
          errs() << "Loop not in LCSSA!\n";
          abort();
        }
      }

      void createPipelineFromStages (LoopDependenceInfo *LDI)
      {
        auto M = LDI->function->getParent();
        LDI->pipelineBB = BasicBlock::Create(M->getContext(), "", LDI->function);
        IRBuilder<> builder(LDI->pipelineBB);

        /*
         * Create and populate the environment and stages arrays
         */
        auto envAlloca = cast<Value>(builder.CreateAlloca(LDI->envArrayType));
        auto envPtr = createEnvArrayFromStages(LDI, builder, envAlloca);
        auto stagesPtr = createStagesArrayFromStages(LDI, builder);

        /*
         * Create empty queues array to be used by the stage dispatcher
         */
        auto queuesAlloca = cast<Value>(builder.CreateAlloca(LDI->queueArrayType));
        auto queuesPtr = cast<Value>(builder.CreateBitCast(queuesAlloca, PointerType::getUnqual(int8)));
        auto queueSizesPtr = createQueueSizesArrayFromStages(LDI, builder);

        /*
         * Call the stage dispatcher with the environment, queues array, and stages array
         */
        auto queuesCount = cast<Value>(ConstantInt::get(int64, LDI->queues.size()));
        auto stagesCount = cast<Value>(ConstantInt::get(int64, LDI->stages.size()));
        builder.CreateCall(stageDispatcher, ArrayRef<Value*>({ envPtr, queuesPtr, queueSizesPtr, stagesPtr, stagesCount, queuesCount }));

        storeOutgoingDependentsIntoExternalValues(LDI, builder, envAlloca);

        /*
         * Branch from pipeline to the correct loop exit block
         */
        auto envIndex = cast<Value>(ConstantInt::get(int64, LDI->environment->envSize() - 1));
        auto depInEnvPtr = builder.CreateInBoundsGEP(envAlloca, ArrayRef<Value*>({ LDI->zeroIndexForBaseArray, envIndex }));
        auto envVarCast = builder.CreateBitCast(builder.CreateLoad(depInEnvPtr), PointerType::getUnqual(int32));
        auto envVar = builder.CreateLoad(envVarCast);

        auto exitSwitch = builder.CreateSwitch(envVar, LDI->loopExitBlocks[0]);
        for (int i = 1; i < LDI->loopExitBlocks.size(); ++i)
        {
          exitSwitch->addCase(ConstantInt::get(int32, i), LDI->loopExitBlocks[i]);
        }
      }

      void linkParallelizedLoopToOriginalFunction (LoopDependenceInfo *LDI)
      {
        auto M = LDI->function->getParent();
        auto preheader = LDI->loop->getLoopPreheader();
        auto loopSwitch = BasicBlock::Create(M->getContext(), "", LDI->function, preheader);
        IRBuilder<> loopSwitchBuilder(loopSwitch);

        auto globalBool = new GlobalVariable(*M, int32, /*isConstant=*/ false, GlobalValue::ExternalLinkage, Constant::getNullValue(int32));
        auto const0 = ConstantInt::get(int32, APInt(32, 0, false));
        auto compareInstruction = loopSwitchBuilder.CreateICmpEQ(loopSwitchBuilder.CreateLoad(globalBool), const0);
        loopSwitchBuilder.CreateCondBr(compareInstruction, LDI->pipelineBB, preheader);
      }

      /*
       * Debug printers:
       */

      void printLoop (Loop *loop)
      {
        errs() << "Applying DSWP on loop\n";
        auto header = loop->getHeader();
        errs() << "Number of bbs: " << std::distance(loop->block_begin(), loop->block_end()) << "\n";
        for (auto bbi = loop->block_begin(); bbi != loop->block_end(); ++bbi){
          auto bb = *bbi;
          if (header == bb) {
            errs() << "Header:\n";
          } else if (loop->isLoopLatch(bb)) {
            errs() << "Loop latch:\n";
          } else if (loop->isLoopExiting(bb)) {
            errs() << "Loop exiting:\n";
          } else {
            errs() << "Loop body:\n";
          }
          for (auto &I : *bb) {
            I.print(errs());
            errs() << "\n";
          }
        }
      }

      void printSCCs (SCCDAG *sccSubgraph)
      {
        errs() << "\nInternal SCCs\n";
        for (auto sccI = sccSubgraph->begin_internal_node_map(); sccI != sccSubgraph->end_internal_node_map(); ++sccI) {
          sccI->first->print(errs());
        }
        errs() << "\nExternal SCCs\n";
        for (auto sccI = sccSubgraph->begin_external_node_map(); sccI != sccSubgraph->end_external_node_map(); ++sccI) {
          sccI->first->print(errs());
        }
        errs() << "Number of SCCs: " << sccSubgraph->numInternalNodes() << "\n";
        for (auto edgeI = sccSubgraph->begin_edges(); edgeI != sccSubgraph->end_edges(); ++edgeI) {
          (*edgeI)->print(errs());
        }
        errs() << "Number of edges: " << std::distance(sccSubgraph->begin_edges(), sccSubgraph->end_edges()) << "\n";
      }

      void printStageSCCs (LoopDependenceInfo *LDI)
      {
        for (auto &stage : LDI->stages)
        {
          errs() << "Stage: " << stage->order << "\n";
          stage->scc->print(errs() << "SCC:\n") << "\n";
          for (auto edge : stage->scc->getEdges()) edge->print(errs()) << "\n";
        }
      }

      void printStageQueues (LoopDependenceInfo *LDI)
      {
        for (auto &stage : LDI->stages)
        {
          errs() << "Stage: " << stage->order << "\n";
          errs() << "Push value queues: ";
          for (auto qInd : stage->pushValueQueues) errs() << qInd << " ";
          errs() << "\nPop value queues: ";
          for (auto qInd : stage->popValueQueues) errs() << qInd << " ";
          errs() << "\n";
        }

        int count = 0;
        for (auto &queue : LDI->queues)
        {
          errs() << "Queue: " << count++ << "\n";
          queue->producer->print(errs() << "Producer:\t"); errs() << "\n";
          for (auto consumer : queue->consumers)
          {
            consumer->print(errs() << "Consumer:\t"); errs() << "\n";
          }
        }
      }
  };

}

// Next there is code to register your pass to "opt"
char llvm::DSWP::ID = 0;
static RegisterPass<DSWP> X("DSWP", "DSWP parallelization");

// Next there is code to register your pass to "clang"
static DSWP * _PassMaker = NULL;
static RegisterStandardPasses _RegPass1(PassManagerBuilder::EP_OptimizerLast,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new DSWP());}}); // ** for -Ox
static RegisterStandardPasses _RegPass2(PassManagerBuilder::EP_EnabledOnOptLevel0,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new DSWP());}});// ** for -O0
