/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The Original Contributor of this Source Code Form is Nokia Research
 * Center Tampere (http://webcl.nokiaresearch.com).
 */

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/User.h"
#include "llvm/IRBuilder.h"
#include "llvm/Operator.h"

#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <cstdio>

using namespace llvm;

static cl::opt<bool>
RunUnsafeMode("allow-unsafe-exceptions",
        cl::desc("Will not add boundary checks to main() arguments and allows calling external functions."),
        cl::init(false), cl::Hidden);

#define UNUSED( x ) \
  (void)x;

/**
 * Exits fast on expected assertion position. Prevents tests from blocking
 * for a few seconds after each tested error case.
 */
#define fast_assert( condition, message ) do {                       \
    if ( condition == false ) {                                      \
      dbgs() << "\nOn line: " << __LINE__ << " " << message << "\n"; \
      exit(1);                                                       \
    }                                                                \
  } while(0)

/**
 * LLVM 3.2 didn't support getAsInstruction() yet
 * so for now we have copypasted it from trunk
 */
Instruction *getAsInstruction(ConstantExpr *expr) {
  SmallVector<Value*,4> ValueOperands;
  for (ConstantExpr::op_iterator I = expr->op_begin(), E = expr->op_end(); I != E; ++I)
    ValueOperands.push_back(cast<Value>(I));
  
  ArrayRef<Value*> Ops(ValueOperands);
  
  switch (expr->getOpcode()) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    return CastInst::Create((Instruction::CastOps)expr->getOpcode(),
                            Ops[0], expr->getType());
  case Instruction::Select:
    return SelectInst::Create(Ops[0], Ops[1], Ops[2]);
  case Instruction::InsertElement:
    return InsertElementInst::Create(Ops[0], Ops[1], Ops[2]);
  case Instruction::ExtractElement:
    return ExtractElementInst::Create(Ops[0], Ops[1]);
  case Instruction::InsertValue:
    return InsertValueInst::Create(Ops[0], Ops[1], expr->getIndices());
  case Instruction::ExtractValue:
    return ExtractValueInst::Create(Ops[0], expr->getIndices());
  case Instruction::ShuffleVector:
    return new ShuffleVectorInst(Ops[0], Ops[1], Ops[2]);
    
  case Instruction::GetElementPtr:
    if (cast<GEPOperator>(expr)->isInBounds())
      return GetElementPtrInst::CreateInBounds(Ops[0], Ops.slice(1));
    else
      return GetElementPtrInst::Create(Ops[0], Ops.slice(1));
    
  case Instruction::ICmp:
  case Instruction::FCmp:
    return CmpInst::Create((Instruction::OtherOps)expr->getOpcode(),
                           expr->getPredicate(), Ops[0], Ops[1]);
    
  default:
    assert(expr->getNumOperands() == 2 && "Must be binary operator?");
    BinaryOperator *BO =
      BinaryOperator::Create((Instruction::BinaryOps)expr->getOpcode(),
                             Ops[0], Ops[1]);
    if (isa<OverflowingBinaryOperator>(BO)) {
      assert(false && "Not supported hopefully never needed until llvm 3.3 is out.");
      //BO->setHasNoUnsignedWrap(expr->SubclassOptionalData &
      //                         OverflowingBinaryOperator::NoUnsignedWrap);
      //BO->setHasNoSignedWrap(expr->SubclassOptionalData &
      //                       OverflowingBinaryOperator::NoSignedWrap);
    }
    if (isa<PossiblyExactOperator>(BO)) {
      assert(false && "Not supported hopefully never needed until llvm 3.3 is out.");
      // BO->setIsExact(expr->SubclassOptionalData & PossiblyExactOperator::IsExact);
    }
    return BO;
  }
}

namespace WebCL {

  /**
   * Returns demangled function name without parameter list.
   *
   * @param name Mangled function name or non-mangled
   * @return Demangled function name or the passed argument if mangling is not recognized.
   */
  std::string extractItaniumDemangledFunctionName(std::string name) {
    bool isMangled = name.find("_Z") == 0;
    std::string retVal = name;
    if (isMangled) {
      size_t lastIndex = name.find_first_not_of("0123456789", 2);
      std::string functionNameLength = name.substr(2, lastIndex-2); 
      fast_assert(functionNameLength.find_first_not_of("0123456789") == std::string::npos, 
                  "Error when trying to demangle: " + name);
      retVal = name.substr(lastIndex, atoi(functionNameLength.c_str()));
    }
    DEBUG( dbgs() << "Demangled: " << name << " to " << retVal << "\n" );
    return retVal;
  }

  /**
   * Creates mangled name (own mangling scheme) to be able to select correct safe builtin to call.
   * All calls to functions with names mangled by this algorithm should be inlined and removed afterwards 
   * by dce.
   * 
   * Scheme steals mangle suffix from original Itanium Mangled function call and add it to our version which
   * is safe to call.
   */
  std::string customMangle(Function* function, std::string base) {
    std::stringstream ss;
    std::string origName = function->getName();
    std::string demangledOrig = extractItaniumDemangledFunctionName(function->getName());
    size_t namePos = origName.find(demangledOrig);
    size_t prefixChars = namePos + demangledOrig.length();
    std::string itaniumMangleSuffix = origName.erase(0, prefixChars);
    ss << base << itaniumMangleSuffix;
    DEBUG( dbgs() << "Orig: " << function->getName() << " new: " << ss.str() << "\n" ); 
    return ss.str();
  }
  
  ConstantInt* getConstInt(LLVMContext &context, int i) {
    return ConstantInt::get(Type::getInt32Ty(context), i);
  }

  template <class T>
  ArrayRef<T> genIntArrayRef(LLVMContext &context, int i1) {
    std::vector<T> temp;
    temp.push_back(getConstInt(context, i1));
    return ArrayRef<T>(temp);
  }

  template <class T>
  ArrayRef<T> genIntArrayRef(LLVMContext &context, int i1, int i2) {
    std::vector<T> temp;
    temp.push_back(getConstInt(context, i1));
    temp.push_back(getConstInt(context, i2));
    return ArrayRef<T>(temp);
  }    
  
  template <class T>
  ArrayRef<T> genIntArrayRef(LLVMContext &context, int i1, int i2, int i3) {
    std::vector<T> temp;
    temp.push_back(getConstInt(context, i1));
    temp.push_back(getConstInt(context, i2));
    temp.push_back(getConstInt(context, i3));
    return ArrayRef<T>(temp);
  }
  
  template <class T>
  ArrayRef<T> genArrayRef(T v1) {
    std::vector<T> temp;
    temp.push_back(v1);
    return ArrayRef<T>(temp);
  }

  template <class T>
  ArrayRef<T> genArrayRef(T v1, T v2) {
    std::vector<T> temp;
    temp.push_back(v1);
    temp.push_back(v2);
    return ArrayRef<T>(temp);
  }    
  
  template <class T>
  ArrayRef<T> genArrayRef(T v1, T v2, T v3) {
    std::vector<T> temp;
    temp.push_back(v1);
    temp.push_back(v2);
    temp.push_back(v3);
    return ArrayRef<T>(temp);
  }    
  
  /// Module pass that implements algorithm for restricting memory
  /// accesses to locally reserved addresses.  
  ///
  /// TODO: add detailed description of algorithm
  /// 

  struct ClampPointers :
    public ModulePass {
    static char ID;

    ClampPointers() :
      ModulePass( ID ) {
    }

    /// An allocation of contiguous array of memory.
    struct SmartPointer {
      SmartPointer( Value* _current, Value* _min, Value* _max, Value* _smart, Value* _smart_ptr ) :
        cur( _current ),
        min( _min ),
        max( _max ),
        smart( _smart ),
        smart_ptr( _smart_ptr ) {
      }

      Value* cur; // Used only to be able to pass pointer value with limits to function call
      Value* min; // Contains first valid address
      Value* max; // Contains last valid address (not last  valid + 1)
      Value* smart;
      Value* smart_ptr;
      
      // returns last valid address for given type
      Instruction* maxFor(LLVMContext &c, LoadInst *maxValue, Type *type) {
        Instruction *retVal = maxValue;
        if (type != maxValue->getType()) {
          DEBUG( dbgs() << "Resolving types "; maxValue->getType()->print(dbgs()); dbgs() << " to "; type->print(dbgs()); dbgs() << "\n" ); 
          // go to end, cast pointer to required type, rewind one elem and return addr 
          // (would be easier if max stores first invalid addr)
          Instruction *firstInvalidAddr = GetElementPtrInst::Create( maxValue, getConstInt(c, 1) );
          Instruction *bitCast = new BitCastInst(firstInvalidAddr, type);
          Instruction *lastValid = GetElementPtrInst::Create( bitCast, getConstInt(c, -1) );          
          lastValid->insertAfter(maxValue);
          bitCast->insertAfter(maxValue);
          firstInvalidAddr->insertAfter(maxValue);

          retVal = lastValid;
          DEBUG( dbgs() << "Resolved: "; retVal->print(dbgs()); dbgs() << " Type: "; retVal->getType()->print(dbgs()); dbgs() << "\n" );
        }
        return retVal;
      }

      // adds type cast to min address if necessary
      Instruction* minFor(LLVMContext &c, LoadInst *minValue, Type *type) {
        Instruction *retVal = minValue;
        if (type != minValue->getType()) {
          DEBUG( dbgs() << "Resolving types "; minValue->getType()->print(dbgs()); dbgs() << " to "; type->print(dbgs()); dbgs() << "\n" ); 
          // cast pointer to required type
          Instruction *bitCast = new BitCastInst(minValue, type);
          bitCast->insertAfter(minValue);
          retVal = bitCast;
        }
        return retVal;
      }

    };

    // area limits for certain value to respect
    // values should be always stored in correct
    // pointer type
    struct AreaLimit {
    private:
      AreaLimit( Value* _min, Value* _max, bool _indirect ) :
      min( _min ),
      max( _max ),
      indirect( _indirect ) {
      }
      
      /**
       * Returns valid address relative to val and offset for certain type.
       *
       * If val is indirect, first add load instruction to get indirect address value and then
       * do the pointer cast / address arithmetic to get the correct address for given type.
       *
       * @param val Direct or indirect base address which from offset is computed
       * @param isIndirect True if we require load before doing offset artihmetics
       * @param offset Offset how many addresses we roll from val
       * @param type Type which kind of pointer we are going to access
       * @param checkStart Position where necessary loads and arithmetict will be added.
       */

      Value* getValidAddressFor(Value *val, bool isIndirect, int offset, Type *type, Instruction *checkStart) {
        
        LLVMContext &c = checkStart->getParent()->getParent()->getContext();
        
        // load indirect value
        Value *limit = val;
        if (isIndirect) {
          limit = new LoadInst(min, "", checkStart);
        }
                
        Value *ret_limit = NULL;
        
        if ( Instruction *inst = dyn_cast<Instruction>(limit) ) {
          // this should be removed by other optimizations if not necessary
          BitCastInst *type_fixed_limit = new BitCastInst(inst, type, "", checkStart);
          ret_limit = GetElementPtrInst::Create(type_fixed_limit, genIntArrayRef<Value*>(c, offset), "", checkStart);
          
        } else if ( Constant *constant = dyn_cast<Constant>(limit) ) {
          Constant *type_fixed_limit = ConstantExpr::getBitCast(constant, type);
          ret_limit = ConstantExpr::getGetElementPtr( type_fixed_limit, genIntArrayRef<Constant*>(c, offset));
          
        } else {
          fast_assert(false, "Couldnt resolve type of the limit value.");
        }
        
        return ret_limit;
      }

    public:
      Value* min; // Contains first valid address
      Value* max; // Contains last valid address
      bool indirect; // true if min and max are indirect pointers (requires load for getting address)

      // TODO: add bookkeeping for freeing memory here
      static AreaLimit* Create( Value* _min, Value* _max, bool _indirect) {
        DEBUG( dbgs() << "Creating limits min: "; _min->print(dbgs()); dbgs() << " max: "; _max->print(dbgs()); dbgs() << " indirect: " << _indirect << "\n"; );
        return new AreaLimit(_min, _max, _indirect);
      }

      /**
       * Returns first valid address inside of these limits for given type of memory access.
       *
       * @param type Type of memory access which is going to be done inside these limits.
       * @param checkStart Instruction before that we add new instructions if necessary.
       */
      Value* firstValidAddressFor(Type *type, Instruction *checkStart) {
        return getValidAddressFor(min, indirect, 0, type, checkStart);
      }

      /**
       * Returns last valid address inside of these limits for given type of memory access.
       *
       * @param type Type of memory access which is going to be done inside these limits.
       * @param checkStart Instruction before that we add new instructions if necessary.
       */
      Value* lastValidAddressFor(Type *type, Instruction *checkStart) {
        return getValidAddressFor(max, indirect, -1, type, checkStart);
      }

    };
      
    typedef std::map< Function*, Function* > FunctionMap;
    typedef std::map< Argument*, Argument* > ArgumentMap;
    typedef std::set< Function* > FunctionSet;
    typedef std::set< Argument* > ArgumentSet;
    typedef std::set< CallInst* > CallInstrSet;
    typedef std::set< AllocaInst* > AllocaInstrSet;
    typedef std::set< GetElementPtrInst* > GepInstrSet;
    typedef std::set< LoadInst* > LoadInstrSet;
    typedef std::set< StoreInst* > StoreInstrSet;
    typedef std::set< Value* > ValueSet;
    typedef std::map< Value*, SmartPointer* > SmartPointerByValueMap;
    typedef std::vector< Value* > ValueVector;
    typedef std::map< unsigned, ValueVector > ValueVectorByAddressSpaceMap; 

    typedef std::set< AreaLimit* > AreaLimitSet;
    typedef std::map< unsigned, AreaLimitSet > AreaLimitSetByAddressSpaceMap;
    typedef std::map< Value*, AreaLimit* > AreaLimitByValueMap;
      
    /// Helper function for generating a single-index GEP instruction from a value.
    GetElementPtrInst* generateGEP( LLVMContext& ctx, Value* ptr, int a, Instruction* i, Twine t = "" ) {
      Twine name = t;
      ConstantInt* c_0 = getConstInt(ctx, a);
      std::vector< Value* > values;
      values.push_back( c_0 );
      ArrayRef< Value* > ref( values );
      GetElementPtrInst* gep = GetElementPtrInst::Create( ptr, ref, name, i );
      return gep;
    }

    /// Helper function for generating a two-index GEP instruction from a value.
    GetElementPtrInst* generateGEP( LLVMContext& c, Value* ptr, int a, int b, Instruction* i, Twine t = "" ) {
      Twine name = t;
      ConstantInt* c_0 = getConstInt(c, a);
      ConstantInt* c_1 = getConstInt(c, b);
      std::vector< Value* > values;
      values.push_back( c_0 );
      values.push_back( c_1 );
      ArrayRef< Value* > ref( values );
      GetElementPtrInst* gep = GetElementPtrInst::Create( ptr, ref, name, i );
      return gep;
    }

    /// Helper function for generating smart pointer struct pointer type
    Type* getSmartPointerType( LLVMContext& c, Type* t ) {
      StructType* s = this->getSmartStructType( c, t );
      Type* smart_array_pointer_type = s->getPointerTo();
      return smart_array_pointer_type;
    }

    /// Helper function for generating smart pointer struct type
    StructType* getSmartStructType( LLVMContext& c, Type* t ) {
      std::vector< Type* > types;
      types.push_back( t );
      types.push_back( t );
      types.push_back( t );

      ArrayRef< Type* > tref( types );

      StructType *smart_array_struct = StructType::get( c, tref );
      assert( smart_array_struct );
      return smart_array_struct;
    }

    /**
     * First analyze original code and then do transformations, 
     * until code is again runnable with using smart pointers.
     * 
     * Then analyse uses of smart pointers to get limits for them and 
     * add boundary checks.
     */
    virtual bool runOnModule( Module &M ) {
      
      // map of functions which has been replaced with new ones
      FunctionMap replacedFunctions;
      // map of function arguments, which are replaced with new ones
      ArgumentMap replacedArguments;
      
      // set of values, which won't need boundary checks to memory accesses
      ValueSet safeExceptions;
      
      // set of different interesting instructions in the program
      // maybe could be just one map of sets sorted by opcode
      CallInstrSet internalCalls;
      CallInstrSet externalCalls;
      AllocaInstrSet allocas;
      StoreInstrSet stores;
      LoadInstrSet loads;
      
      // smartpointers sorted by value e.g. i32* %a ->  {i32*, i32*, i32*}* %safe_a
      SmartPointerByValueMap smartPointers;

      // limits for values and address spaces
      AreaLimitByValueMap valueLimits;
      AreaLimitSetByAddressSpaceMap addressSpaceLimits;

      // TODO: MAYBE THIS COULD BE MOVED TO COMPLETELY SEPARATE PASS
      DEBUG( dbgs() << "\n --------------- COLLECT ALLOCAS AND GLOBALS AND CREATE ONE BIG STRUCT --------------\n" );
      consolidateStaticMemory( M );

      DEBUG( dbgs() << "\n --------------- FIND LIMITS FOR EACH ADDRESS SPACE --------------\n" );
      findAddressSpaceLimits( M, addressSpaceLimits );

      
      // ####### Analyze all functions
      for( Module::iterator i = M.begin(); i != M.end(); ++i ) {

        // allow calling external functions with original signatures (this pass should be ran just 
        // for fully linked code)
        if ( i->isIntrinsic() || i->isDeclaration() ) {         
          if (RunUnsafeMode) {
            DEBUG( dbgs() << "Skipping: " << i->getName() << " which is intrinsic and/or declaration\n" );
            continue;
          }

          // if ok builtin, skip this function (only analyze functions which has implementation available)
          if (!isWebClBuiltin(i)) {
            dbgs() << "Found: " << i->getName() << " which is intrinsic and/or declaration\n";
            fast_assert(false, "Calling external functions is not allowed in strict mode. "
                        "Also intrinsics should be lowered before runnin pass.");
          } else {
            DEBUG( dbgs() << "Recognized builtin: "; i->print(dbgs()); );
            continue;
          }
        }

        // actually this should just collect functions to replace, but now it also creates new signatures
        DEBUG( dbgs() << "\n --------------- CREATING NEW FUNCTION SIGNATURE --------------\n" );
        createNewFunctionSignature( i, replacedFunctions, replacedArguments );

        DEBUG( dbgs() << "\n --------------- FINDING INTERESTING INSTRUCTIONS --------------\n" );
        sortInstructions( i,  internalCalls, externalCalls, allocas, stores, loads );
      }
      
      DEBUG( dbgs() << "\n ----------- CONVERTING OLD FUNCTIONS TO NEW ONES AND FIXING SMART POINTER ARGUMENT PASSING  ----------\n" );
      // gets rid of old functions, replaces calls to old functions and fix call arguments to 
      // use smart pointers in call parameters
      moveOldFunctionImplementationsToNewSignatures(replacedFunctions, replacedArguments, smartPointers); // TODO: remove unused smartPointers argument
      
      DEBUG( dbgs() << "\n --------------- CREATE KERNEL ENTRY POINTS AND GET ADDITIONAL LIMITS FROM KERNEL ARGUMENTS --------------\n" );
      createKernelEntryPoints(M, replacedFunctions, addressSpaceLimits);

      DEBUG( dbgs() << "\n --------------- FIND LIMITS OF EVERY LOAD/STORE OPERAND --------------\n" );
      findLimits(replacedFunctions, loads, stores, valueLimits, addressSpaceLimits);
      
      DEBUG( dbgs() << "\n --------------- FIX CALLS TO USE NEW SIGNATURES --------------\n" );
      fixCallsToUseChangedSignatures(replacedFunctions, replacedArguments, internalCalls, smartPointers);
      
      // ##########################################################################################
      // #### At this point code should be again perfectly executable and runs with new function 
      // #### signatures and has cahnged pointers to smart ones
      // ##########################################################################################

      DEBUG( dbgs() << "\n --------------- ANALYZING CODE TO FIND SPECIAL CASES WHERE CHECKS ARE NOT NEEDED TODO: collect direct loads etc. who shouldnt need checks --------------\n" );
      collectSafeExceptions(replacedFunctions, safeExceptions);

      DEBUG( dbgs() << "\n --------------- ADDING BOUNDARY CHECKS --------------\n" );
      addBoundaryChecks(stores, loads, valueLimits, addressSpaceLimits, safeExceptions);

      DEBUG( dbgs() << "\n --------------- FIX BUILTIN CALLS TO CALL SAFE VERSIONS IF NECESSARY --------------\n" );
      makeBuiltinCallsSafe(externalCalls, smartPointers);

      DEBUG( dbgs() << "\n --------------- FINAL OUTPUT --------------\n" );
      M.print(dbgs(), NULL);
      DEBUG( dbgs() << "\n --------------- FINAL OUTPUT END --------------\n" );

      return true;
    }
    
    /**
     * Resolves uses of value and limits that it should respect
     *
     * Do simple data dependency analysis to be able to resolve limits which
     * values should respect in case of same address space has more 
     * than allocated 1 areas.
     *
     * Follows uses of val and in case of storing to memory, keep track if
     * there is always only single limits for that location.
     */
    void resolveUses(Value *val, AreaLimitByValueMap &valLimits, AreaLimitSetByAddressSpaceMap &asLimits) {
      
      // check all uses of value until cannot trace anymore
      for( Value::use_iterator i = val->use_begin(); i != val->use_end(); ++i ) {
        Value *use = *i;
        
        // ----- continue to next use if cannot be sure about the limits
        if ( dyn_cast<GetElementPtrInst>(use) ) {
          DEBUG( dbgs() << "Found GEP: "; use->print(dbgs()); dbgs() << "\n## Preserving original limits KEEP ON TRACKING\n"; );
        } else if ( LoadInst *load = dyn_cast<LoadInst>(use) ) {
          DEBUG( dbgs() << "Found LOAD: "; use->print(dbgs()); dbgs() << "\n## If we reached here we should have already resolved limits from somewhere. Keep on resolving.\n"; );
        } else if ( StoreInst *store = dyn_cast<StoreInst>(use) ) {
          DEBUG( dbgs() << "Found STORE: "; use->print(dbgs()); dbgs() << "\n## If we are storing pointer, also pass VAL limits to destination address. DEPENDENCY ANALYSIS\n" );
          
          // first check if use is actually in value operand and in that case set limits for destination pointer
          if (store->getValueOperand() == val) {
            if (valLimits.count(store->getPointerOperand()) != 0) {
              fast_assert(valLimits[store->getPointerOperand()] == valLimits[val],
                          "Dependency analysis cannot resolve single limits for a memory address. This is a bit nasty problem to resolve, since we cannot pass multiple possible limits to functions safe pointer argument. SPIR + removing all safe pointer argument hassling could help this some day. For now avoid assigning pointers from different ranges to the same variable.");
            }
            valLimits[store->getPointerOperand()] = valLimits[val];
            resolveUses(store->getPointerOperand(), valLimits, asLimits);
          }
          continue;
        
        } else if ( BitCastInst* bitCast = dyn_cast<BitCastInst>(use) ) {
          // if bitcast is not from pointer to pointer in same address space, cannot resolve
          if (!bitCast->getType()->isPointerTy() ||
              dyn_cast<PointerType>(bitCast->getType())->getAddressSpace() != dyn_cast<PointerType>(val->getType())->getAddressSpace()) {
            DEBUG( dbgs() << "## Found bitcast that cannot preserve limits.\n" );
            continue;
          }
          DEBUG( dbgs() << "## Found valid pointer cast, keep on tracking.\n" );
        
        } else {
          // notify about unexpected cannot be resolved cases for debug
          DEBUG( dbgs() << "#### Cannot resolve limit for: "; use->print(dbgs()); dbgs() << "\n" );
          continue;
        }
        
        // limits of use are directly derived from value
        valLimits[use] = valLimits[val];
        resolveUses(use, valLimits, asLimits);
      }
    }
  
    /**
     * Goes through all relevant parts in program and traces limits for those values.
     *
     * Call operands are not a problem anymore, since they has been converted to pass structs, not direct pointers.
     */
    void findLimits(FunctionMap &replacedFunctions,
                    LoadInstrSet &loads,
                    StoreInstrSet &stores,
                    AreaLimitByValueMap &valLimits,
                    AreaLimitSetByAddressSpaceMap &asLimits) {
    
      // first trace all uses of function arguments to find their limits
      for ( FunctionMap::iterator i = replacedFunctions.begin(); i != replacedFunctions.end(); i++ )  {
        Function *originalFunc = i->first;
        Function *safePointerFunction = i->second;
        Function::arg_iterator originalArgIter = originalFunc->arg_begin();

        for( Function::arg_iterator a = safePointerFunction->arg_begin(); a != safePointerFunction->arg_end(); ++a ) {
          Argument &originalArg = *originalArgIter;
          Argument &replaceArg = *a;
          
          // if safe pointer argument, trace uses
          if (originalArg.getType() != replaceArg.getType()) {

            fast_assert(replaceArg.getNumUses() == 1, "Safe pointer argument should have only one extractval use so far.");
            ExtractValueInst *cur = dyn_cast<ExtractValueInst>(*replaceArg.use_begin());
            fast_assert(cur, "Found invalid type of use. Maybe passed directly to other function.");
            
            // Adding extract value instructions to entry block to have direct limits stored.
            BasicBlock &entry = safePointerFunction->getEntryBlock();
            ExtractValueInst *minLimit = ExtractValueInst::Create( &replaceArg, genArrayRef<unsigned int>(1), replaceArg.getName() + ".min", &(*entry.begin()) );
            ExtractValueInst *maxLimit = ExtractValueInst::Create( &replaceArg, genArrayRef<unsigned int>(2), replaceArg.getName() + ".max", &(*entry.begin()) );
            
            // Init direct limits for current and do some analysis to resolve derived limits
            valLimits[cur] = AreaLimit::Create(minLimit, maxLimit, false);
            resolveUses(cur, valLimits, asLimits);
          }
          
          originalArgIter++;
        }
      }
      
      ValueSet operands;
      // collect values whose limits to trace
      for (LoadInstrSet::iterator i = loads.begin(); i != loads.end(); i++) {
        LoadInst *load = *i;
        operands.insert(load->getPointerOperand());
      }
      for (StoreInstrSet::iterator i = stores.begin(); i != stores.end(); i++) {
        StoreInst *store = *i;
        operands.insert(store->getPointerOperand());
      }
      
      // optimize single area address space limits
      for (ValueSet::iterator i = operands.begin(); i != operands.end(); i++) {
        Value* val = *i;
        DEBUG( dbgs() << "Tracing limits for: "; val->print(dbgs()); dbgs() << "\n"; );
        PointerType *t = dyn_cast<PointerType>(val->getType());
        AreaLimitSet &limitSet = asLimits[t->getAddressSpace()];
        fast_assert(limitSet.size() > 0, "Pointer to address space without allocations.");
        if ( limitSet.size() == 1 ) {
          DEBUG( dbgs() << "Found single limits for AS: " << t->getAddressSpace() << "\n"; );
          valLimits[val] = *(limitSet.begin());
        }
      }
    }
      
    /**
     * Goes through global variables and adds limits to bookkeepping
     */
    void findAddressSpaceLimits( Module &M, AreaLimitSetByAddressSpaceMap &asLimits ) {
      LLVMContext& c = M.getContext();
      for (Module::global_iterator g = M.global_begin(); g != M.global_end(); g++) {

        // collect only named addresses (for unnamed there cannot be relative references anywhere)
        if (!g->hasUnnamedAddr()) {
          DEBUG( dbgs() << "AS: " << g->getType()->getAddressSpace() << " Added global: "; g->print(dbgs()); dbgs() << "\n"; );
          Constant *firstValid = ConstantExpr::getGetElementPtr(g, genIntArrayRef<Constant*>(c, 0));
          Constant *firstInvalid = ConstantExpr::getGetElementPtr(g, genIntArrayRef<Constant*>(c, 1));
          asLimits[g->getType()->getAddressSpace()].insert(AreaLimit::Create(firstValid, firstInvalid, false));
        }
      }
    }
      
    /**
     * Collect all allocas and global values for each address space and create one struct for each
     * address space.
     */
    void consolidateStaticMemory( Module &M ) {
      LLVMContext& c = M.getContext();
      
      ValueVectorByAddressSpaceMap staticAllocations;

      for (Module::global_iterator g = M.global_begin(); g != M.global_end(); g++) {
        // collect only named addresses (for unnamed there cannot be relative references anywhere)
        if (!g->hasUnnamedAddr()) {
          DEBUG( dbgs()  << "Found global: "; g->print(dbgs());
                 dbgs() << " of address space: " << g->getType()->getAddressSpace() << "\n"; );
          staticAllocations[g->getType()->getAddressSpace()].push_back(g);
        }
      }

      for (Module::iterator f = M.begin(); f != M.end(); f++) {
        // skip declarations (they does not even have entry blocks)
        if (f->isDeclaration()) continue;
        BasicBlock &entry = f->getEntryBlock();
        for (BasicBlock::iterator i = entry.begin(); i != entry.end(); i++) {
          AllocaInst *alloca = dyn_cast<AllocaInst>(i);
          if (alloca != NULL) {
            staticAllocations[alloca->getType()->getAddressSpace()].push_back(alloca);
          }
        }
      }
      
      // create struct for each address space, currently not doing any special ordering 
      for (ValueVectorByAddressSpaceMap::iterator i = staticAllocations.begin(); i != staticAllocations.end(); i++) {
        unsigned addressSpace = i->first;
        std::vector<Value*> &values = i->second;
        // TODO: sort types by alignment and size to minimize padding

        // create struct type
        std::vector< Type* > structElementTypes;
        std::vector< Constant* > structInitData;
        for (size_t valIndex = 0; valIndex < values.size(); valIndex++) {

          // element type 
          Value* val = values[valIndex];
          structElementTypes.push_back(dyn_cast<PointerType>(val->getType())->getElementType());
          
          // initializer
          Type* elementType = NULL;
          Constant* initializer = NULL;
          if ( AllocaInst* alloca = dyn_cast<AllocaInst>(val) ) {
            elementType = alloca->getType()->getElementType();
          } else if ( GlobalVariable* global = dyn_cast<GlobalVariable>(val) ) {
            elementType = global->getType()->getElementType();
            if (global->hasInitializer()) {
              initializer = global->getInitializer();
              global->setInitializer(NULL);
            }
          } else {
            dbgs() << "Got unexpected static allocation: "; val->print(dbgs()); dbgs() << "\n";
            fast_assert(false, "Unexpected type static allocation.");
          }

          if (!initializer) {
            if (elementType->isAggregateType()) {
              structInitData.push_back(ConstantAggregateZero::get(elementType));
            } else {
              structInitData.push_back(Constant::getNullValue(elementType));
            }
          } else {
            structInitData.push_back(initializer);
          }
        }

        ArrayRef< Type* > structElementTypesArrayRef( structElementTypes );
        ArrayRef<Constant*> structElementData( structInitData );

        
        std::stringstream structName;
        structName << "AddressSpace" << addressSpace << "StaticData";
        StructType *addressSpaceStructType = StructType::get( c, structElementTypesArrayRef );
        // NOTE: cant give name to struct literal type, would be nice to have for readability
        // addressSpaceStructType->setName(structName.str() + "Type");

        // create struct of generated type and add to module
        Constant* addressSpaceDataInitializer = ConstantStruct::get( addressSpaceStructType, structElementData );
        GlobalVariable *aSpaceStruct = new GlobalVariable
          (M, addressSpaceStructType, false, GlobalValue::InternalLinkage, addressSpaceDataInitializer, 
           structName.str(), NULL, GlobalVariable::NotThreadLocal, addressSpace);
        
        // replace all uses of old allocas and globals value with new constant geps and remove original values
        for (size_t valIndex = 0; valIndex < values.size(); valIndex++) {
          Value* origVal = values[valIndex];

          // get field of struct
          Value* structVal = ConstantExpr::getInBoundsGetElementPtr
            (dyn_cast<Constant>(aSpaceStruct), genIntArrayRef<Constant*>( c, 0, valIndex) );

          structVal->takeName(origVal);
             
          DEBUG( dbgs() << "Orig val type: "; origVal->getType()->print(dbgs()); 
                 dbgs() << " new val type: "; structVal->getType()->print(dbgs()); dbgs() << "\n"; );
          DEBUG( dbgs() << "Orig val: "; origVal->print(dbgs()); 
                 dbgs() << " new val: "; structVal->print(dbgs()); dbgs() << "\n"; );

          origVal->replaceAllUsesWith(structVal);

          if ( AllocaInst* alloca = dyn_cast<AllocaInst>(origVal) ) {
            alloca->eraseFromParent();
          } else if ( GlobalVariable* global = dyn_cast<GlobalVariable>(origVal) ) {
            global->eraseFromParent();
          }
        }
      }
    }

    /**
     * Checks if given function declaration is one of webcl builtins
     * 
     * NOTE: This check and validity that only builtins are called can be implemented easier
     * by setting compiler to give an error if call to undefined function is made. All builtins
     * are defined already in some implicit kernel header.
     *
     * Since this is does not really matter here, return always true
     */
    bool isWebClBuiltin(Function *F) {
      return true;
    }

    /**
     * Goes through kernels metadata entries and creates webcl compliant kernel signature.
     *
     * If signature has no pointers, then do nothing, if there was pinter arguments, add 
     * count parameter after each pointer to tell how many elements pointer has. Take original
     * kernel name and add implementation, that just resolves the last address of array and passes
     * it as limit to safepointer version of original kernel.
     */
    void createKernelEntryPoints(Module &M, FunctionMap &replacedFunctions, AreaLimitSetByAddressSpaceMap &asLimits) {
      NamedMDNode* oclKernels = M.getNamedMetadata("opencl.kernels");
      if (oclKernels != NULL) {
        for (unsigned int op = 0; op < oclKernels->getNumOperands(); op++) {
          MDNode* md = oclKernels->getOperand(op);
          DEBUG( dbgs() << "Fixing entry point " << op << ": "; md->print(dbgs()); dbgs() << " --> " );
          Function* oldFun = dyn_cast<Function>(md->getOperand(0));
          Function *newKernelEntryFunction = NULL;
          
          // If there is need, create new kernel wrapper and replace old kernel reference with new WebCl
          // compatible version.
          if (replacedFunctions.count(oldFun) > 0) {
            Function *smartKernel = replacedFunctions[oldFun];
            newKernelEntryFunction = createWebClKernel(M, oldFun, smartKernel, asLimits);
            // make smartKernel to be internal linkage to allow better optimization
            smartKernel->setLinkage(GlobalValue::InternalLinkage);
            // TODO: if found nvptx_kernel attribute, move it to new kernel
            md->replaceOperandWith(0, newKernelEntryFunction);
          }

          DEBUG( md->print(dbgs()); dbgs() << "\n" );
        }
      }
    }

    /**
     * Creates new WebCl kernel compliant function, which has element count parameter for each
     * pointer parameter and can be called from host.
     *
     * New function will be given name of the original kernel, but after each pointer parameter,
     * count parameter will be added which is used to pass information how many elements are
     * reserved in pointer. Function implementation will convert (pointer, count) to corresponding
     * smart pointer, which is used to make call to smartKernel.
     */
    Function* createWebClKernel(Module &M, Function *origKernel, Function *smartKernel, AreaLimitSetByAddressSpaceMap &asLimits) {
      LLVMContext &c = M.getContext();

      // create argument list for WebCl kernel
      std::vector< Type* > paramTypes;
      for( Function::arg_iterator a = origKernel->arg_begin(); a != origKernel->arg_end(); ++a ) {
        Argument* arg = a;
        Type* t = arg->getType();
        paramTypes.push_back( t );
        
        if( t->isPointerTy() ) {
          Type* arraySizeType = Type::getInt32Ty(c);
          paramTypes.push_back( arraySizeType );          
        }
      }

      // creating new function with WebCl compatible arguments
      FunctionType *functionType = origKernel->getFunctionType();
      FunctionType *newFunctionType = FunctionType::get( functionType->getReturnType(), paramTypes, false );
      Function *webClKernel = dyn_cast<Function>( M.getOrInsertFunction("", newFunctionType) );
      webClKernel->takeName( origKernel );

      // create basic block and builder
      BasicBlock* kernelBlock = BasicBlock::Create( c, "entry", webClKernel );
      IRBuilder<> blockBuilder( kernelBlock );

      std::vector<Value*> args;
      Function::arg_iterator origArg = origKernel->arg_begin();
      for( Function::arg_iterator a = webClKernel->arg_begin(); a != webClKernel->arg_end(); ++a ) {
        Argument* arg = a;
        arg->setName(origArg->getName());
        Type* t = arg->getType();
        
        if ( t->isPointerTy() ) {
          // create global unnamed variables for each limits got from kernel arguments
          GlobalVariable *globalMin = new GlobalVariable( M, t, false, GlobalValue::PrivateLinkage, Constant::getNullValue(t) );
          GlobalVariable *globalMax = new GlobalVariable( M, t, false, GlobalValue::PrivateLinkage, Constant::getNullValue(t) );
          globalMin->setUnnamedAddr(true);
          globalMax->setUnnamedAddr(true);
          PointerType *pointerType = dyn_cast<PointerType>(t);
          
          // add addresses to limit set for the address space
          DEBUG( dbgs() << "AS: " << pointerType->getAddressSpace() << " Adding indirect limits from kernel parameter: "; arg->print(dbgs()); dbgs() << "\n"; );
          asLimits[pointerType->getAddressSpace()].insert( AreaLimit::Create(globalMin, globalMax, true) );
          
          Value* elementCount = (++a);
          a->setName(origArg->getName() + ".size");
          GetElementPtrInst *lastLimit = dyn_cast<GetElementPtrInst>(blockBuilder.CreateGEP(arg, elementCount));

          blockBuilder.CreateStore(arg, globalMin);
          blockBuilder.CreateStore(lastLimit, globalMax);
          
          // create smart pointer alloca to entry block of function, which is used as a argument to
          // function call
          Value* newArgument = convertArgumentToSmartStruct( arg,  arg, lastLimit, kernelBlock);

          args.push_back(newArgument);

        } else {
          args.push_back(arg);
        }
        origArg++;
      }

      DEBUG( dbgs() << "\nCreated arguments: "; 
             for ( size_t i = 0; i < args.size(); i++ ) { 
               args[i]->getType()->print(dbgs()); dbgs() << " "; 
             } dbgs() << "\n"; ) ;
      DEBUG( dbgs() << "Function arguments: "; 
             for ( Function::arg_iterator a = smartKernel->arg_begin(); a != smartKernel->arg_end(); ++a ) { 
               a->getType()->print(dbgs()); dbgs() << " "; 
             } dbgs() << "\n"; ) ;
      
      blockBuilder.CreateCall(smartKernel, args);
      blockBuilder.CreateRetVoid();

      DEBUG( webClKernel->print(dbgs()) );
      return webClKernel;
    }

    /**
     * Converts old argument to SafePointer with limits added.
     *
     * TODO: maybe we could generalize this and add some bookkeeping even that these really are 
     *       used only inside function scope. But Would be nice to say that update smart pointer
     *       which will store new values to argument struct just before function call.
     *
     *       Maybe it wont be needed and I can just skip messing with function signatures altogether..
     *
     * TODO: or just maybe we could create unnamed global variable and pass it to prevent polluting entry block too much
     */
    Value* convertArgumentToSmartStruct(Value* origArg, Value* minLimit, Value* maxLimit, Instruction *beforeInstruction) {
      DEBUG( dbgs() << "1-Converting arg: "; origArg->print(dbgs()); dbgs() << " min: "; minLimit->print(dbgs()); dbgs() << " max: "; maxLimit->print(dbgs()); dbgs() << "\n"; );
      // create alloca to entry block of function for the value
      Function* argFun = beforeInstruction->getParent()->getParent();
      BasicBlock &entryBlock = argFun->getEntryBlock();
      LLVMContext &c = argFun->getContext();
      Type *smarArgType = getSmartStructType(c, origArg->getType());
      AllocaInst *smartArgStructAlloca = new AllocaInst(smarArgType, origArg->getName() + ".SmartPassing", &entryBlock);
      
      // create temp smart pointer struct and initialize it with correct values
      GetElementPtrInst *curGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 0), "", beforeInstruction);
      GetElementPtrInst *minGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 1), "", beforeInstruction);
      GetElementPtrInst *maxGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 2), "", beforeInstruction);
      new StoreInst(origArg, curGEP, beforeInstruction);
      new StoreInst(minLimit, minGEP, beforeInstruction);
      new StoreInst(maxLimit, maxGEP, beforeInstruction);
      LoadInst *smartArgVal = new LoadInst(smartArgStructAlloca, "", beforeInstruction);
      return smartArgVal;
    }

    // copy - paste refactor !
    Value* convertArgumentToSmartStruct(Value* origArg, Value* minLimit, Value* maxLimit, BasicBlock *initAtEndOf) {
      DEBUG( dbgs() << "2-Converting arg: "; origArg->print(dbgs()); dbgs() << " min: "; minLimit->print(dbgs()); dbgs() << " max: "; maxLimit->print(dbgs()); dbgs() << "\n"; );      
      // create alloca to entry block of function for the value
      Function* argFun = initAtEndOf->getParent();
      BasicBlock &entryBlock = argFun->getEntryBlock();
      LLVMContext &c = argFun->getContext();
      Type *smarArgType = getSmartStructType(c, origArg->getType());
      AllocaInst *smartArgStructAlloca = new AllocaInst(smarArgType, origArg->getName() + ".SmartPassing", &entryBlock);
      
      // create temp smart pointer struct and initialize it with correct values
      GetElementPtrInst *curGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 0), "", initAtEndOf);
      GetElementPtrInst *minGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 1), "", initAtEndOf);
      GetElementPtrInst *maxGEP = GetElementPtrInst::CreateInBounds(smartArgStructAlloca, genIntArrayRef<Value*>(c, 0, 2), "", initAtEndOf);
      new StoreInst(origArg, curGEP, initAtEndOf);
      new StoreInst(minLimit, minGEP, initAtEndOf);
      new StoreInst(maxLimit, maxGEP, initAtEndOf);
      LoadInst *smartArgVal = new LoadInst(smartArgStructAlloca, "", initAtEndOf);
      return smartArgVal;
    }
      
    /**
     * Paint all uses of argv of main function as safe ones, which does not require checks.
     *
     * NOTE: should never be called for opencl code
     */
    void resolveArgvUses(Value *val, ValueSet &safeExceptions) {
      for( Value::use_iterator i = val->use_begin(); i != val->use_end(); ++i ) {
        Value *use = *i;
        
        // ----- continue to next use if cannot be sure about if still safe
        if ( dyn_cast<GetElementPtrInst>(use) || dyn_cast<LoadInst>(use) ) {
          DEBUG( dbgs() << "Use: "; use->print(dbgs()); dbgs() << " is safe!\n" );
          safeExceptions.insert(use);
          resolveArgvUses(use, safeExceptions);                
        } else if ( StoreInst *store = dyn_cast<StoreInst>(use) ) {

          // dont care about store, but try to find it's destinations uses too
          if (safeExceptions.count(store->getPointerOperand()) == 0 && store->getPointerOperand()->getName() == "argv.addr") {
            DEBUG( dbgs() << "store has no uses, but follow its destination's uses: "; use->print(dbgs()); dbgs() << "\n" );
            DEBUG( dbgs() << "follow: "; store->getPointerOperand()->print(dbgs()); dbgs() << "\n" );
            safeExceptions.insert(store->getPointerOperand());
            resolveArgvUses(store->getPointerOperand(), safeExceptions);
          }
        } else { 
          // notify about unexpected cannot be resolved cases for debug
          DEBUG( dbgs() << "Cannot resolve if still safe for: "; use->print(dbgs()); dbgs() << "\n" );
          continue;
        }
      }
    }

    /**
     * Collects values, which can be handled without modifying.
     * 
     * e.g. main function arguments (int8** is not currently supported 
     * and won't be in the first place).
     *
     * Note: this is quite dirty symbol name based hack...
     */
    void collectSafeExceptions(FunctionMap &replacedFunctions, ValueSet &safeExceptions) {
      if (RunUnsafeMode) {
        for ( FunctionMap::iterator i = replacedFunctions.begin(); i != replacedFunctions.end(); i++ )  {
          Function *check = i->second;
          if (check->getName() == "main__smart_ptrs__") {
            check->takeName(i->first);
            for( Function::arg_iterator a = check->arg_begin(); a != check->arg_end(); ++a ) {
              Argument* arg = a;
              if (arg->getName() == "argv") {
                resolveArgvUses(arg, safeExceptions);
              }
            }
          }
        }
      } else {
        DEBUG( dbgs() << "No exceptions added in strict mode. \n" );
      }
    }
    
    /**
     * Returns true if pointer limits can be safely resolved for the global value.
     *
     * Used to whitelist supported IR constructs.
     */
    bool isSafeGlobalValue(GlobalValue *g) {
      
      // if address of global value is not important 
      if ( g->hasUnnamedAddr() ) {
        return true;
      }
      
      // if variable is internal and we should be able to resolve safely its limits
      if ( g->hasInternalLinkage() ) {
        return true;
      }

      return false;
    }
    

    /**
     * Checks if store stores data to smart pointer and updates also smart pointer accordingly.
     */
    void addBoundaryChecks(StoreInstrSet &stores, LoadInstrSet &loads, AreaLimitByValueMap &valLimits, AreaLimitSetByAddressSpaceMap &asLimits, ValueSet &safeExceptions) {
      // check load instructions... 
      for (LoadInstrSet::iterator i = loads.begin(); i != loads.end(); i++) {
        addChecks((*i)->getPointerOperand(), *i, valLimits, asLimits, safeExceptions);
      }   
      // check store instructions
      for (StoreInstrSet::iterator i = stores.begin(); i != stores.end(); i++) {
        addChecks((*i)->getPointerOperand(), *i, valLimits, asLimits, safeExceptions);
      }
    }

    /**
     * If val touching pointer operand needs checks, then inject boundary check code.
     */
    void addChecks(Value *ptrOperand, Instruction *inst, AreaLimitByValueMap &valLimits, AreaLimitSetByAddressSpaceMap &asLimits, ValueSet &safeExceptions) {
      
      // If no need to add checks, just skip
      if (safeExceptions.count(ptrOperand)) {
        DEBUG( dbgs() << "Skipping op that was listed in safe exceptions: "; inst->print(dbgs()); dbgs() << "\n" );        
        return;
      }
      
      AreaLimitSet limits;
      if ( valLimits.count(ptrOperand) != 0 ) {
        limits.insert(valLimits[ptrOperand]);
      } else {
        unsigned int addressSpace = dyn_cast<PointerType>(ptrOperand->getType())->getAddressSpace();
        limits.insert( asLimits[addressSpace].begin(), asLimits[addressSpace].end() );
      }
      
      createLimitCheck(ptrOperand, limits, inst);
    }
    
    /**
     * Adds boundary check for given pointer
     *
     * ==== Changes e.g.
     * 
     * %0 = load i32** %some_label
     * %1 = load i32* %0
     *
     * ==== To
     *
     *   %0 = load i32** %some_label
     *   ; this checks if value is direct or indirect and does required casting and gets last valid address for clamp
     *   %1 = AreaLimit.getMaxFor(%some_label)
     *   %2 = AreaLimit.getMinFor(%some_label)
     *   %3 = icmp ugt i32* %0, %1
     *   br i1 %3, label %boundary.check.fail, label %check.first.limit
     * check.first.limit:      
     *   %4 = icmp ult i32* %0, %2
     *   br i1 %4, label %boundary.check.fail, label %boundary.check.ok
     * boundary.check.ok:
     *   %5 = load i32* %0
     *   br %if.end
     * boundary.check.fail:
     *   br %if.end
     * if.end:
     *   %6 = phi i32* [ 0, %boundary.check.fail ], [ %5, %boundary.check.ok ]
     *
     * ==== for store instruction phi node is not generated (instruction is just skipped)
     * 
     * @param ptr Address whose limits are checked
     * @param limits Smart pointer, whose limits pointer should respect
     * @param meminst Instruction which for check is injected
     */
    void createLimitCheck(Value *ptr, AreaLimitSet &limits, Instruction *meminst) {
      
      DEBUG( dbgs() << "Creating limit check for: "; ptr->print(dbgs()); dbgs() << " of type: "; ptr->getType()->print(dbgs()); dbgs() << "\n" );
      static int id = 0;
      id++;
      char postfix_buf[64];
    
      if ( dyn_cast<LoadInst>(meminst) ) {
        sprintf(postfix_buf, "load.%d", id);
      } else {
        sprintf(postfix_buf, "store.%d", id);
      }
      std::string postfix = postfix_buf;
      
      DEBUG( dbgs() << "Limits to check: \n" );
      for (AreaLimitSet::iterator i = limits.begin(); i != limits.end(); i++) {
        DEBUG( dbgs() << "### min: "; (*i)->min->print(dbgs()); dbgs() << "\n"; );
        DEBUG( dbgs() << "### max: "; (*i)->max->print(dbgs()); dbgs() << "\n"; );
      }
      fast_assert(limits.size() == 1, "Current boundary check generation does not support multiple limits checking.");
      AreaLimit *limit = *(limits.begin());
      
      BasicBlock *BB = meminst->getParent();
      Function *F = BB->getParent();
      LLVMContext& c = F->getContext();

      // ------ this block is destination of all places where limit check fails, needs unconditional just branch to if.end block
      BasicBlock* boundary_fail_block = BasicBlock::Create( c, "boundary.check.failed." + postfix, F );
      IRBuilder<> boundary_fail_builder( boundary_fail_block );

      // ------ block for minimum value check
      BasicBlock* check_first_block = BasicBlock::Create( c, "check.first.limit." + postfix, F );
      IRBuilder<> check_first_builder( check_first_block );

      // ------ get limits if require loading indirect address

      // *   %1 = instruction or value returning last valid value
      Value *last_value_for_type = limit->lastValidAddressFor(ptr->getType(), meminst);
      // *   %2 = value to compare to get first valid address
      Value *first_valid_pointer = limit->firstValidAddressFor(ptr->getType(), meminst);

      // ------ add max boundary check code

      // get limits for this certain type of pointer... basically does "((ptrType)(&last_val[1]))[-1]"
      DEBUG( last_value_for_type->getType()->print(dbgs()); dbgs() << " VS. "; ptr->getType()->print(dbgs()); dbgs() << "\n" );

      // *   %3 = icmp ugt i32* %0, %1
      ICmpInst* cmp = new ICmpInst( meminst, CmpInst::ICMP_UGT, ptr, last_value_for_type, "" );
      // *   br i1 %3, label %boundary.check.failed, label %check.first.limit
      BranchInst::Create( boundary_fail_block, check_first_block, cmp, meminst );

      // ------ break current BB to 3 parts, start, boundary_check_ok and if_end (meminst is left in ok block)

      // ------ this block actually contains the load/store instruction and branch to if.end block
      BasicBlock* boundary_ok_block = BB->splitBasicBlock(meminst, "boundary.check.ok." + postfix);

      // leave meminst to ok block and split it again to create if.end block
      BasicBlock* end_block = 
        boundary_ok_block->splitBasicBlock(boundary_ok_block->begin()->getNextNode(),
                                           "if.end.boundary.check." + postfix);
      
      // erase implicitly added branch from start block to boundary.check.ok
      BB->back().eraseFromParent();

      // and add unconditional branch from boundary_fail_block to if.end 
      BranchInst::Create( end_block, boundary_fail_block );

      // ------ add min boundary check code
      // * check.first.limit:

      // *   %4 = icmp ult i32* %0, %2
      ICmpInst* cmp2 = new ICmpInst( *check_first_block, CmpInst::ICMP_ULT, ptr, first_valid_pointer, "" );

      // *   br i1 %4, label %boundary.check.failed, label %if.end
      BranchInst::Create( boundary_fail_block, boundary_ok_block, cmp2, check_first_block );

      // if meminst == load, create phi node to start of if.end block and replace all uses of meminst with this phi
      if ( dyn_cast<LoadInst>(meminst) ) {
        PHINode* newPhi = PHINode::Create(meminst->getType(), 2, "", &end_block->front());
        meminst->replaceAllUsesWith(newPhi);
        newPhi->addIncoming(meminst, boundary_ok_block);
        newPhi->addIncoming(Constant::getNullValue(meminst->getType()), boundary_fail_block);
      }

      // organize blocks to order shown in comment
      check_first_block->moveAfter(BB);
      boundary_ok_block->moveAfter(check_first_block);
      boundary_fail_block->moveAfter(boundary_ok_block);
      end_block->moveAfter(boundary_fail_block);

      DEBUG( dbgs() << "Created boundary check for: "; meminst->print(dbgs()); dbgs() << "\n"; );
    }

    /**
     * Goes through external function calls and if call is unsafe opencl call convert it to safe webcl implementation
     * which operates with smart pointers
     */
    void makeBuiltinCallsSafe(CallInstrSet &calls, SmartPointerByValueMap &smartPointers) {
      std::string unsafeBuiltins_tmp[] = {
        "fract", "frexp", "lgamma_r", "modf", "remquo", "sincos", 
        "vload2", "vload3", "vload4", "vload8", "vload16", 
        "vstore2", "vstore3", "vstore4", "vstore8", "vstore16",
        "async_work_group_copy",
        "async_work_group_strided_copy",
        "wait_group_events",
        "atomic_add", "atomic_sub", "atomic_xchg", 
        "atomic_inc", "atomic_dec", "atomic_cmpxchg", 
        "atomic_min", "atomic_max",
        "atomic_and", "atomic_or", "atomic_xor"
      };
      std::set<std::string> unsafeBuiltins
        (unsafeBuiltins_tmp, unsafeBuiltins_tmp + sizeof(unsafeBuiltins_tmp) / sizeof(unsafeBuiltins_tmp[0]));

      // we currently do not support some half vstore/vload variants
      std::string forbiddenBuiltins_tmp[] = {
        "vload_half", "vload_half2", "vload_half3", "vload_half4", "vload_half8", "vload_half16", 
        "vloada_half2", "vloada_half3", "vloada_half4", "vloada_half8", "vloada_half16", 
        "vstore_half", "vstore_half2", "vstore_half3", "vstore_half4", "vstore_half8", "vstore_half16", 
        "vstore_half_rte", "vstore_half2_rte", "vstore_half3_rte", "vstore_half4_rte", "vstore_half8_rte", "vstore_half16_rte",
        "vstore_half_rtz", "vstore_half2_rtz", "vstore_half3_rtz", "vstore_half4_rtz", "vstore_half8_rtz", "vstore_half16_rtz",
        "vstore_half_rtp", "vstore_half2_rtp", "vstore_half3_rtp", "vstore_half4_rtp", "vstore_half8_rtp", "vstore_half16_rtp",
        "vstore_half_rtn", "vstore_half2_rtn", "vstore_half3_rtn", "vstore_half4_rtn", "vstore_half8_rtn", "vstore_half16_rtn",
        "vstorea_half2", "vstorea_half3", "vstorea_half4", "vstorea_half8", "vstorea_half16", 
        "vstorea_half2_rte", "vstorea_half3_rte", "vstorea_half4_rte", "vstorea_half8_rte","vstorea_half16_rte",
        "vstorea_half2_rtz", "vstorea_half3_rtz", "vstorea_half4_rtz", "vstorea_half8_rtz","vstorea_half16_rtz",
        "vstorea_half2_rtp", "vstorea_half3_rtp", "vstorea_half4_rtp", "vstorea_half8_rtp","vstorea_half16_rtp",
        "vstorea_half2_rtn", "vstorea_half3_rtn", "vstorea_half4_rtn", "vstorea_half8_rtn","vstorea_half16_rtn"
      };
      std::set<std::string> forbiddenBuiltins
        (forbiddenBuiltins_tmp, forbiddenBuiltins_tmp + sizeof(forbiddenBuiltins_tmp) / sizeof(forbiddenBuiltins_tmp[0]));


      // if mapping is needed outside export this to be reference parameter instead of local 
      FunctionMap safeBuiltins;
      ArgumentMap dummyArgMap;

      for (CallInstrSet::iterator i = calls.begin(); i != calls.end(); i++) {
        CallInst *call = *i;

        DEBUG( dbgs() << "---- Checking builtin call:"; call->print(dbgs()); dbgs() << "\n" );
        
        Function* oldFun = call->getCalledFunction();
        
        if ( isWebClBuiltin(oldFun) ) {

          std::string demangledName = extractItaniumDemangledFunctionName(oldFun->getName().str());

          // if not supported yet assert
          fast_assert( forbiddenBuiltins.count(demangledName) == 0, 
                       "Tried to call forbidden builtin: " + oldFun->getName() + " " + demangledName);
          
          // if unsafe fix call
          if ( unsafeBuiltins.count(demangledName) > 0 ) {
            
            // if safe version is not yet generated do it first..
            if ( safeBuiltins.count(oldFun) == 0 ) {
              Function *newFun = createNewFunctionSignature(oldFun, safeBuiltins, dummyArgMap);
              // simple name mangler to be able to select, which implementation to call (couldn't find easy way to do Itanium C++ mangling here)
              // luckily the cases that needs mangling are pretty limited so we can keep it simple
              newFun->setName(customMangle(oldFun, demangledName + "__safe__"));
            }
            
            Function *newFun = safeBuiltins[oldFun];
            ArgumentMap dummyArg;
            convertCallToUseSmartPointerArgs(call, newFun, dummyArg);
          }

        } else {
          if ( RunUnsafeMode ) {
            dbgs() << "WARNING: Calling external function, which we cannot guarantee to be safe: "; 
            oldFun->print(dbgs());
            continue;
          } else {
            fast_assert(false, "Aborting since we are in strict mode.");
          }
        }
      }
    }
    
    /**
     * Goes through function calls and change call parameters to be suitable for new function signature.
     *
     * Also updates param.Cur value before making call to make sure that smart pointer has always the latest 
     * value stored.
     */
    void fixCallsToUseChangedSignatures(FunctionMap &replacedFunctions, 
                                        ArgumentMap &replacedArguments, 
                                        CallInstrSet &calls, 
                                        SmartPointerByValueMap &smartPointers) {

      for (CallInstrSet::iterator i = calls.begin(); i != calls.end(); i++) {
        CallInst *call = *i;

        DEBUG( dbgs() << "---- Started fixing:"; call->print(dbgs()); dbgs() << "\n" );
        
        Function* oldFun = call->getCalledFunction();
        
        // if function was not replaced (didn't have pointer parameters)
        if (replacedFunctions.count(oldFun) == 0) {
          continue;
        }

        Function* newFun = replacedFunctions[oldFun];
        convertCallToUseSmartPointerArgs(call, newFun, replacedArguments);
      }
    }
    
    /**
     * Converts call function to use new function as called function and changes all pointer parameters to be smart pointers.
     */
    void convertCallToUseSmartPointerArgs(CallInst *call, Function *newFun, ArgumentMap &replacedArguments) {

      Function* oldFun = call->getCalledFunction();
      call->setCalledFunction(newFun);
      
      // find if function signature changed some Operands and change them to refer smart pointers 
      // instead of pointers directly
      int op = 0;
      Function::arg_iterator newArgIter = newFun->arg_begin();
      for( Function::arg_iterator a = oldFun->arg_begin(); a != oldFun->arg_end(); ++a ) {
        Argument* oldArg = a;
        Argument* newArg = newArgIter;
        newArgIter++;
        
        // NOTE: If we would first expand smart pointer map, we might be able to resolve smart pointer for parameter
        //       a lot easier... if there is need to add more and more special cases here, consider the option...

        // this argument type has been changed to smart pointer, find out corresponding smart
        if (oldArg->getType() != newArg->getType()) {
          Value* operand = call->getArgOperand(op);
          
          DEBUG( dbgs() << "- op #" << op << " needs fixing: "; operand->print(dbgs()); dbgs() << "\n" );
          
          if ( Argument* arg = dyn_cast<Argument>(operand) ) {
            // if operand is argument it should be found from replacement map
            DEBUG( dbgs() << "Operand is argument of the same func!\n"; );
            call->setOperand( op, replacedArguments[arg] );

          } else if (ExtractValueInst *extract = dyn_cast<ExtractValueInst>(operand)) {
            // TODO: REMOVE THIS HACK IT OPENS SECURITY HOLE, ALWAYS GET LIMITS FROM RESULT OF ANALYSIS
            //       THIS WILL ALLOW UNSAFE CODE IF STRUCT IS GIVEN AS ARGUMENT AND THEN ONE ELEMENT OF
            //       IT IS PASSED TO OTHER FUNCTION
            Value* aggregateOp = extract->getAggregateOperand();
            DEBUG( dbgs() << "Operand is extractval of argument of the same func: "; aggregateOp->print(dbgs()); dbgs() << "\n"; );
            call->setOperand( op, aggregateOp );
          
          } else {
            // TODO: find limits of this operand
            // TODO: if not found, get limits of address space
            // TODO: if multiple limits, set NULL limits and complain to user that their code might be a bit unefficient
            // TODO: initialize safe pointer with limits and pass it to call
            // for now, we just always pass NULL pointers to limits and check against address space limits
            DEBUG( dbgs() << "Need to create limits for operand: "; operand->print(dbgs()); dbgs() << "\n"; );
            call->setOperand( op, convertArgumentToSmartStruct( operand, NULL, NULL, call) );
            fast_assert(false, "NEVER PASS NULLS AS LIMITS, ANALYSIS SHOULD ALREADY KNOW LIMITS! ");
          }
        }
        op++;
      }
      
      DEBUG( dbgs() << "-- Converted call to : "; call->print(dbgs()); dbgs() << "\n" );
    }
    
    /**
     * Goes through all replaced functions and their arguments.
     *
     * 1. Moves all basic blocks to new function
     * 2. For each argument if necessary adds exctractvalue instruction to get passed pointer value
     * 3. Replaces all uses of old function argument with extractvalue instruction or with new function argument if it was not pointer.
     */
    void moveOldFunctionImplementationsToNewSignatures(FunctionMap &replacedFunctions, 
                                                       ArgumentMap &replacedArguments, 
                                                       SmartPointerByValueMap &smartPointers) {
            
      for (FunctionMap::iterator i = replacedFunctions.begin(); i != replacedFunctions.end(); i++) {
        // loop through arguments and if type has changed, then create label to access original arg 
        Function* oldFun = i->first;
        Function* newFun = i->second;
        
        LLVMContext& c = oldFun->getContext();
        
        // move all instructions to new function
        newFun->getBasicBlockList().splice( newFun->begin(), oldFun->getBasicBlockList() );
        BasicBlock &entryBlock = newFun->getEntryBlock();
 
        DEBUG( dbgs() << "Moved BBs to " << newFun->getName() << "( .... ) and took the final function name.\n" );

        for( Function::arg_iterator a = oldFun->arg_begin(); a != oldFun->arg_end(); ++a ) {
          Argument* oldArg = a;
          Argument* newArg = replacedArguments[a];

          DEBUG( dbgs() << "Fixing arg: "; oldArg->print(dbgs()); dbgs() << " : " );

          newArg->takeName(oldArg);
          oldArg->setName(newArg->getName() + ".orig");
          
          // non safe pointer argument... direct replace
          if (oldArg->getType() == newArg->getType()) {
            DEBUG( dbgs() << "type was not changed. Just replacing oldArg uses with newArg.\n" );
            // argument was not tampered, just replace uses to point the new function
            oldArg->replaceAllUsesWith(newArg);
            continue;
          }

          // if argument types are not the same we need to find smart pointer that was generated for
          // argument and create initializations so that existing smart alloca will get correct values

          // argument types are not the same we need to get .Cur element of the passed safe pointer, which is being
          // used in function and replace all uses with that.

          DEBUG( dbgs() << "1 newArg: "; newArg->print(dbgs()); );
          Twine paramName = Twine("") + newArg->getName() + ".SmartArg";
          newArg->setName(paramName);

          // get value of passed smart_pointer.cur and replace all uses of original argument with it
          ExtractValueInst* newArgCur = ExtractValueInst::Create(newArg,
                                                                 genArrayRef<unsigned int>(0),
                                                                 Twine("") + newArg->getName() + ".Cur",
                                                                 entryBlock.begin());

          // this potentially will not work if there is store to arg... probably that case is impossible to happen and smart pointer arguments are read-only
          DEBUG( dbgs() << "Replacing old arg: "; oldArg->getType()->print(dbgs()); dbgs() << " with: "; newArgCur->getType()->print(dbgs()); dbgs() << "\n"; );
          oldArg->replaceAllUsesWith(newArgCur);
                    
        } // -- end arguments for loop
      }  
    }
    
    /**
     * Creates new function signatures and mapping between original and new.
     *
     * Function does not modify function yet in any manner.
     *
     * If kernel function is seen, we should assert (@todo needs to be implemented when llvm 3.2 is ready),
     * until we are ready with implementing safe pointer construction from (int*, int) pairs.
     * 
     * If int main(int argc, char *argv[]) add to safe functions and safe arguments or assert because of those parameters for now.
     * 
     */
    virtual Function* createNewFunctionSignature( 
      Function *F,  
      FunctionMap &functionMapping, 
      ArgumentMap &argumentMapping ) {
       
      LLVMContext& c = F->getContext();

      // currently returning pointer or array is not supported
      fast_assert( (!F->getFunctionType()->getReturnType()->isPointerTy()), 
                   "Handling function returning pointer is not implemented." );
      fast_assert( (!F->getFunctionType()->getReturnType()->isArrayTy()), 
                   "Handling function returning array type is not implemented." );
      fast_assert( (!F->isVarArg()), "Variable argument functions are not supported.");

      // check if main or kernel and in that case do not change signature 
      bool dontTouchArguments = false;

      if (RunUnsafeMode && F->getName() == "main") {
        dontTouchArguments = true;
      }
      
      // convert function signature to use pointer structs instead of direct pointers
      std::vector< Type* > param_types;
      for( Function::arg_iterator a = F->arg_begin(); a != F->arg_end(); ++a ) {
        Argument* arg = a;
        Type* t = arg->getType();

        // TODO: assert not supported arguments (e.g. some int**, struct etc... or at least verify cases we can allow)
        
        if( !dontTouchArguments && t->isPointerTy() ) {
          Type* smart_array_struct = getSmartStructType( c, t );
          param_types.push_back( smart_array_struct );          
        } else {
          fast_assert( (!t->isArrayTy()), "Passing array in arguments is not implemented." );
          param_types.push_back( t );
        }
      }

      // creating new function with different prototype 
      FunctionType *function_type = F->getFunctionType();
      FunctionType *new_function_type = FunctionType::get( function_type->getReturnType(), param_types, false );

      Function *new_function = Function::Create( new_function_type, F->getLinkage() );
      new_function->copyAttributesFrom( F );      
      F->getParent()->getFunctionList().insert( F, new_function );
      new_function->setName( F->getName() + "__smart_ptrs__" );
      
      // add new function to book keepig to show what was replaced
      functionMapping.insert( std::pair< Function*, Function* >( F, new_function ) );

      DEBUG( dbgs() << "-- Created new signature for: " << F->getName() << " "; F->getType()->print(dbgs()) );
      DEBUG( dbgs() << "\nnew signature: " << new_function->getName() << " "; new_function->getType()->print(dbgs()); dbgs() << "\n" );

      // map arguments of original function to new replacements
      for( Function::arg_iterator 
             a = F->arg_begin(), 
             E = F->arg_end(), 
             a_new = new_function->arg_begin(); a != E; ++a, ++a_new ) {     
        
        argumentMapping.insert( std::pair< Argument*, Argument* >( a, a_new ) ); 
        DEBUG( dbgs() << "Mapped orig arg: "; a->print(dbgs()); dbgs() << " -----> "; a_new->print(dbgs()); dbgs() << "\n" );
      }

      return new_function;
    }

    virtual void sortInstructions( Function *F, 
                                   CallInstrSet &internalCalls, 
                                   CallInstrSet &externalCalls, 
                                   AllocaInstrSet &allocas,
                                   StoreInstrSet &stores,
                                   LoadInstrSet &loads) {
      
      DEBUG( dbgs() << "-- Finding interesting instructions from: " << F->getName() << "\n" );

      // find all instructions which should be handled afterwards
      for ( Function::iterator bb = F->begin(); bb != F->end(); bb++) {
        for( BasicBlock::iterator i = bb->begin(); i != bb->end(); ++i ) {      
          Instruction &inst = *i;
       
          if ( CallInst *call = dyn_cast< CallInst >(&inst) ) {
            if (!call->getCalledFunction()->isIntrinsic()) {
              if (call->getCalledFunction()->isDeclaration()) {
                externalCalls.insert(call);
                DEBUG( dbgs() << "Found external call: " );
              } else {
                internalCalls.insert(call);
                DEBUG( dbgs() << "Found internal call: " );
              }
              DEBUG( call->print(dbgs()); dbgs() << "\n" );
            } else {
              DEBUG( dbgs() << "Ignored call to intrinsic\n" );
            }

          } else if ( AllocaInst *alloca = dyn_cast< AllocaInst >(&inst) ) {
            
            // TODO: check if alloca is from smart pointer argument. 
            // ( for these we should no do traditional smart pointer initialization, 
            //  but initialize them from sp read from argument )
            
            allocas.insert(alloca);
            DEBUG( dbgs() << "Found alloca: "; alloca->print(dbgs()); dbgs() << "\n" );
            
          } else if ( StoreInst *store = dyn_cast< StoreInst >(&inst) ) {
            
            if (dyn_cast<Argument>(store->getValueOperand())) {
              DEBUG( dbgs() << "Skipping store which reads function argument: "; store->print(dbgs()); dbgs() << "\n" );
              continue;
            } 
            
            stores.insert(store);
            DEBUG( dbgs() << "Found store: "; store->print(dbgs()); dbgs() << "\n" );
            
          } else if ( LoadInst *load = dyn_cast< LoadInst >(&inst) ) {            
            
            loads.insert(load);
            DEBUG( dbgs() << "Found load: "; load->print(dbgs()); dbgs() << "\n" );

          } else if ( dyn_cast<FenceInst>(&inst) || 
                      dyn_cast<VAArgInst>(&inst) ||  
                      dyn_cast<AtomicRMWInst>(&inst) || 
                      dyn_cast<AtomicCmpXchgInst>(&inst) ) {
            
            DEBUG( dbgs() << "Unsafe instruction: "; inst.print(dbgs()); dbgs() << "\n" );
            fast_assert(false, "Instruction is not supported.");
          } 
        }
      }
    }
  };
}
  
char WebCL::ClampPointers::ID = 0;
static RegisterPass<WebCL::ClampPointers> 
X("clamp-pointers", "Adds dynamic checks to prevent accessing memory outside of allocated area.", 
  false, false);

