/*
 * Copyright 2016 - 2021  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "noelle/core/SystemHeaders.hpp"
#include "noelle/core/InductionVariables.hpp"
#include "noelle/core/LoopGoverningIVAttribution.hpp"

namespace llvm::noelle {

  class IVUtility {
    public:

      static PHINode *createChunkPHI (
          BasicBlock *preheaderBlock,
          BasicBlock *headerBlock,
          Type *chunkPHIType,
          Value *chunkSize
          ) ;

      static void chunkInductionVariablePHI (
          BasicBlock *preheaderBlock,
          PHINode *ivPHI,
          PHINode *chunkPHI,
          Value *chunkStepSize
          ) ;

      static Value *offsetIVPHI (
          BasicBlock *insertBlock,
          PHINode *ivPHI,
          Value *startValue,
          Value *offsetValue
          );

      static void stepInductionVariablePHI (
          BasicBlock *preheaderBlock,
          PHINode *ivPHI,
          Value *additionalStepSize
          );

  };

  class LoopGoverningIVUtility {
    public:

      LoopGoverningIVUtility (
        LoopStructure *loopOfGIV, 
        InductionVariableManager &IVM,
        LoopGoverningIVAttribution &GIV
        ) ;

      LoopGoverningIVUtility () = delete ;

      std::vector<Instruction *> &getConditionValueDerivation (void) ;

      void updateConditionAndBranchToCatchIteratingPastExitValue (
          CmpInst *cmpToUpdate,
          BranchInst *branchInst,
          BasicBlock *exitBlock
          ) ;

      void updateConditionToCheckIfWeHavePastExitValue (
          CmpInst *cmpToUpdate
          ) ;

      void cloneConditionalCheckFor (
          Value *recurrenceOfIV,
          Value *clonedComparedValue,
          BasicBlock *continueBlock,
          BasicBlock *exitBlock,
          IRBuilder<> &cloneBuilder
          ) ;

      Value * generateCodeToComputeTheTripCount (
        IRBuilder<> &builder
        );

      /*
       * @return Value of the IV that is used to compare against the exit condition value of the loop
       */
      Value * generateCodeToComputePreviousValueUsedToCompareAgainstExitConditionValue (
          IRBuilder<> &builder,
          Value *currentIterationValue,
          BasicBlock *latch,
          Value *stepValue
        );

      /*
       * @return Value of the IV that is used to compare against the exit condition value of the loop
       */
      Value * generateCodeToComputeValueToUseForAnIterationAgo (
          IRBuilder<> &builder,
          Value *currentIterationValue,
          Value *stepValue
        );

    private:
      LoopStructure *loop;
      LoopGoverningIVAttribution &attribution;
      CmpInst *condition;
      std::vector<Instruction *> conditionValueOrderedDerivation;

      CmpInst::Predicate nonStrictPredicate;
      CmpInst::Predicate strictPredicate;
      bool doesOriginalCmpInstHaveIVAsLeftOperand;
      bool flipOperandsToUseNonStrictPredicate;
      bool flipBrSuccessorsToUseNonStrictPredicate;
      bool isWhile;
  };

}
