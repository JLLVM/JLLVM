// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include "Interpreter.hpp"

#include <jllvm/class/ByteCodeIterator.hpp>

#include "VirtualMachine.hpp"

jllvm::Interpreter::Interpreter(VirtualMachine& virtualMachine, std::uint64_t backEdgeThreshold)
    : m_virtualMachine(virtualMachine),
      m_backEdgeThreshold(backEdgeThreshold),
      m_jit2InterpreterSymbols(
          m_virtualMachine.getRuntime().getJITCCDylib().getExecutionSession().createBareJITDylib("<jit2interpreter>")),
      m_interpreterCCSymbols(m_jit2InterpreterSymbols.getExecutionSession().createBareJITDylib("<interpreterSymbols>")),
      m_compiled2InterpreterLayer(virtualMachine.getRuntime().getInterner(),
                                  virtualMachine.getRuntime().getLLVMIRLayer(),
                                  virtualMachine.getRuntime().getDataLayout())
{
    m_interpreterCCSymbols.addToLinkOrder(virtualMachine.getRuntime().getCLibDylib());
    m_jit2InterpreterSymbols.addToLinkOrder(m_interpreterCCSymbols);
    m_jit2InterpreterSymbols.addToLinkOrder(virtualMachine.getRuntime().getClassAndMethodObjectsDylib());
    m_jit2InterpreterSymbols.addToLinkOrder(virtualMachine.getRuntime().getCLibDylib());

    m_virtualMachine.getRuntime().addImplementationSymbols(
        m_interpreterCCSymbols,
        std::pair{"jllvm_interpreter",
                  [&](const Method* method, std::uint16_t* byteCodeOffset, std::uint16_t* topOfStack,
                      std::uint64_t* operandStack, std::uint64_t* operandGCMask, std::uint64_t* localVariables,
                      std::uint64_t* localVariablesGCMask)
                  {
                      InterpreterContext context(*topOfStack, operandStack, operandGCMask, localVariables,
                                                 localVariablesGCMask);
                      return executeMethod(*method, *byteCodeOffset, context);
                  }},
        std::pair{"jllvm_interpreter_frame_sizes",
                  [](const Method* method, std::uint16_t* numLocals, std::uint16_t* numOperands)
                  {
                      Code* code = method->getMethodInfo().getAttributes().find<Code>();
                      *numLocals = code->getMaxLocals();
                      *numOperands = code->getMaxStack();
                  }},
        std::pair{
            "jllvm_interpreter_init_locals",
            [](const Method* method, const std::uint64_t* arguments, std::uint64_t* locals, std::uint64_t* localsGCMask)
            {
                std::size_t argumentIndex = 0;
                if (!method->isStatic())
                {
                    locals[argumentIndex] = arguments[argumentIndex];
                    MutableBitArrayRef<>(localsGCMask, argumentIndex + 1)[argumentIndex] = true;
                    argumentIndex++;
                }

                for (FieldType fieldType : method->getType().parameters())
                {
                    locals[argumentIndex] = arguments[argumentIndex];
                    if (fieldType.isReference())
                    {
                        MutableBitArrayRef<>(localsGCMask, argumentIndex + 1)[argumentIndex] = true;
                    }
                    argumentIndex++;
                    if (fieldType.isWide())
                    {
                        argumentIndex++;
                    }
                }
            }},
        std::pair{"jllvm_osr_frame_delete", [](const std::uint64_t* osrFrame) { delete[] osrFrame; }});

    generateInterpreterEntry();
    m_interpreterInterpreterCCOSREntry = generateOSREntry("V", CallingConvention::Interpreter);
    m_interpreterJITCCOSREntryReferenceReturn = generateOSREntry("Ljava/lang/Object;", CallingConvention::JIT);
    for (BaseType::Values value : {BaseType::Boolean, BaseType::Char, BaseType::Float, BaseType::Double, BaseType::Byte,
                                   BaseType::Short, BaseType::Int, BaseType::Long, BaseType::Void})
    {
        m_interpreterJITCCOSREntries[value - BaseType::MinValue] =
            generateOSREntry(BaseType(value), CallingConvention::JIT);
    }
}

namespace
{
llvm::Value* divideCeil(llvm::IRBuilder<>& builder, llvm::Value* value, llvm::Value* rhs)
{
    // This is a 'ceil(value / rhs)' operation implemented in IR as "value / rhs + ((value % rhs) != 0)".
    llvm::Value* remainder = builder.CreateURem(value, rhs);
    value = builder.CreateUDiv(value, rhs);
    return builder.CreateAdd(
        value, builder.CreateZExt(builder.CreateICmpNE(remainder, llvm::ConstantInt::get(value->getType(), 0)),
                                  value->getType()));
}

void callGetFrameSize(llvm::IRBuilder<>& builder, llvm::Value* methodRef, llvm::Value* numLocalsPtr,
                      llvm::Value* numOperandsPtr)
{
    llvm::Module* module = builder.GetInsertBlock()->getParent()->getParent();
    // Get the number of local variables and operand stack slots from the runtime.
    // The method uses two out parameters rather than a struct of two as the latter does not automatically adhere to the
    // C ABI in LLVM.
    builder.CreateCall(
        module->getOrInsertFunction(
            "jllvm_interpreter_frame_sizes",
            llvm::FunctionType::get(builder.getVoidTy(),
                                    {methodRef->getType(), numLocalsPtr->getType(), numOperandsPtr->getType()},
                                    /*isVarArg=*/false)),
        {methodRef, numLocalsPtr, numOperandsPtr});
}
} // namespace

void jllvm::Interpreter::generateInterpreterEntry()
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("module", *context);
    module->setDataLayout(m_compiled2InterpreterLayer.getDataLayout());
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    llvm::StringRef functionName = "jllvm_interpreter_entry";
    auto* pointer = llvm::PointerType::get(*context, 0);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt64Ty(*context), {pointer, pointer}, /*isVarArg=*/false),
        llvm::GlobalValue::ExternalLinkage, functionName, module.get());

    TrivialDebugInfoBuilder debugInfoBuilder(function);

    applyABIAttributes(function);
    function->clearGC();
    addJavaInterpreterMethodMetadata(function, CallingConvention::Interpreter);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    llvm::Value* methodRef = function->getArg(0);
    llvm::Value* callerArguments = function->getArg(1);

    llvm::AllocaInst* byteCodeOffset = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* topOfStack = builder.CreateAlloca(builder.getInt16Ty());

    llvm::AllocaInst* numLocalsPtr = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* numOperandsPtr = builder.CreateAlloca(builder.getInt16Ty());
    callGetFrameSize(builder, methodRef, numLocalsPtr, numOperandsPtr);

    llvm::Value* numLocals =
        builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), numLocalsPtr), builder.getInt32Ty());
    llvm::Value* numOperands =
        builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), numOperandsPtr), builder.getInt32Ty());

    // Allocate the stack variables now that we know their sizes.
    llvm::AllocaInst* operandStack = builder.CreateAlloca(builder.getInt64Ty(), numOperands);
    llvm::AllocaInst* operandGCMask =
        builder.CreateAlloca(builder.getInt64Ty(), divideCeil(builder, numOperands, builder.getInt32(64)));
    llvm::AllocaInst* localVariables = builder.CreateAlloca(builder.getInt64Ty(), numLocals);
    llvm::AllocaInst* localVariablesGCMask =
        builder.CreateAlloca(builder.getInt64Ty(), divideCeil(builder, numLocals, builder.getInt32(64)));

    builder.CreateMemSet(byteCodeOffset, builder.getInt8(0), sizeof(std::uint16_t), std::nullopt);
    builder.CreateMemSet(topOfStack, builder.getInt8(0), sizeof(std::uint16_t), std::nullopt);

    // These three technically do not have to be zeroed, but we do so anyway for ease of debugging and security.
    // Can be changed in the future for performance.
    builder.CreateMemSet(operandStack, builder.getInt8(0),
                         builder.CreateMul(numOperands, builder.getInt32(sizeof(std::uint64_t))), std::nullopt);
    builder.CreateMemSet(operandGCMask, builder.getInt8(0),
                         builder.CreateMul(operandGCMask->getArraySize(), builder.getInt32(sizeof(std::uint64_t))),
                         std::nullopt);
    builder.CreateMemSet(localVariables, builder.getInt8(0),
                         builder.CreateMul(numLocals, builder.getInt32(sizeof(std::uint64_t))), std::nullopt);

    builder.CreateMemSet(
        localVariablesGCMask, builder.getInt8(0),
        builder.CreateMul(localVariablesGCMask->getArraySize(), builder.getInt32(sizeof(std::uint64_t))), std::nullopt);

    // Initialize the local variables from the argument array.
    builder.CreateCall(module->getOrInsertFunction(
                           "jllvm_interpreter_init_locals",
                           llvm::FunctionType::get(builder.getVoidTy(),
                                                   {methodRef->getType(), callerArguments->getType(),
                                                    localVariables->getType(), localVariablesGCMask->getType()},
                                                   /*isVarArg=*/false)),
                       {methodRef, callerArguments, localVariables, localVariablesGCMask});

    std::array<llvm::Value*, 7> arguments = {methodRef,     byteCodeOffset, topOfStack,          operandStack,
                                             operandGCMask, localVariables, localVariablesGCMask};
    std::array<llvm::Type*, 7> types{};
    llvm::transform(arguments, types.begin(), std::mem_fn(&llvm::Value::getType));

    // Deopt all values used as context during interpretation. This makes it possible for the unwinder to read the
    // method, local variables, the operand stack, the bytecode offset and where GC pointers are contained during
    // unwinding.
    llvm::CallInst* callInst = builder.CreateCall(
        module->getOrInsertFunction("jllvm_interpreter",
                                    llvm::FunctionType::get(builder.getInt64Ty(), types, /*isVarArg=*/false)),
        arguments, llvm::OperandBundleDef("deopt", arguments));
    builder.CreateRet(callInst);

    debugInfoBuilder.finalize();

    Runtime& runtime = m_virtualMachine.getRuntime();
    llvm::cantFail(runtime.getLLVMIRLayer().add(m_interpreterCCSymbols,
                                                llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    m_interpreterEntry = reinterpret_cast<decltype(m_interpreterEntry)>(
        llvm::cantFail(runtime.getSession().lookup({&m_interpreterCCSymbols}, runtime.getInterner()(functionName)))
            .getAddress());
}

void* jllvm::Interpreter::generateOSREntry(FieldType returnType, CallingConvention callingConvention)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("module", *context);
    module->setDataLayout(m_compiled2InterpreterLayer.getDataLayout());
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    std::string functionName = "Interpreter OSR Entry";
    switch (callingConvention)
    {
        case CallingConvention::Interpreter: functionName += " Interpreter CC"; break;
        case CallingConvention::JIT: functionName += " JIT CC " + returnType.textual(); break;
    }
    auto* function = llvm::Function::Create(osrMethodSignature(returnType, callingConvention, *context),
                                            llvm::GlobalValue::ExternalLinkage, functionName, module.get());

    TrivialDebugInfoBuilder debugInfoBuilder(function);

    applyABIAttributes(function);
    function->clearGC();
    addJavaInterpreterMethodMetadata(function, callingConvention);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    llvm::AllocaInst* byteCodeOffset = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* topOfStack = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* numLocalsPtr = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* numOperandsPtr = builder.CreateAlloca(builder.getInt16Ty());

    auto* pointer = llvm::PointerType::get(*context, 0);
    llvm::Value* currOsrState = function->getArg(0);
    llvm::Value* methodRef = builder.CreateLoad(pointer, currOsrState);

    currOsrState = builder.CreateConstGEP1_32(builder.getInt64Ty(), currOsrState, 1);
    builder.CreateStore(builder.CreateLoad(builder.getInt16Ty(), currOsrState), byteCodeOffset);
    llvm::Value* topOfStackValue = builder.CreateLoad(builder.getInt32Ty(), currOsrState);
    topOfStackValue = builder.CreateLShr(topOfStackValue, 16);
    builder.CreateStore(builder.CreateTrunc(topOfStackValue, builder.getInt16Ty()), topOfStack);

    callGetFrameSize(builder, methodRef, numLocalsPtr, numOperandsPtr);

    llvm::Value* numLocals =
        builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), numLocalsPtr), builder.getInt32Ty());
    llvm::Value* numOperands =
        builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), numOperandsPtr), builder.getInt32Ty());

    // Allocate the stack variables now that we know their sizes.
    llvm::AllocaInst* operandStack = builder.CreateAlloca(builder.getInt64Ty(), numOperands);
    llvm::AllocaInst* operandGCMask =
        builder.CreateAlloca(builder.getInt64Ty(), divideCeil(builder, numOperands, builder.getInt32(64)));
    llvm::AllocaInst* localVariables = builder.CreateAlloca(builder.getInt64Ty(), numLocals);
    llvm::AllocaInst* localVariablesGCMask =
        builder.CreateAlloca(builder.getInt64Ty(), divideCeil(builder, numLocals, builder.getInt32(64)));

    // Initialize the interpreter state from the OSR State.
    currOsrState = builder.CreateConstGEP1_32(builder.getInt64Ty(), currOsrState, 1);
    builder.CreateMemCpy(localVariables, /*DstAlign=*/std::nullopt, currOsrState,
                         /*SrcAlign=*/std::nullopt,
                         builder.CreateMul(numLocals, builder.getInt32(sizeof(std::uint64_t))));

    currOsrState = builder.CreateGEP(builder.getInt64Ty(), currOsrState, numLocals);
    builder.CreateMemCpy(operandStack, /*DstAlign=*/std::nullopt, currOsrState, /*SrcAlign=*/std::nullopt,
                         builder.CreateMul(topOfStackValue, builder.getInt32(sizeof(std::uint64_t))));

    currOsrState = builder.CreateGEP(builder.getInt64Ty(), currOsrState, topOfStackValue);
    builder.CreateMemCpy(
        localVariablesGCMask, /*DstAlign=*/std::nullopt, currOsrState, /*SrcAlign=*/std::nullopt,
        builder.CreateMul(localVariablesGCMask->getArraySize(), builder.getInt32(sizeof(std::uint64_t))));

    currOsrState = builder.CreateGEP(builder.getInt64Ty(), currOsrState, localVariablesGCMask->getArraySize());
    builder.CreateMemCpy(operandGCMask, /*DstAlign=*/std::nullopt, currOsrState, /*SrcAlign=*/std::nullopt,
                         builder.CreateMul(operandGCMask->getArraySize(), builder.getInt32(sizeof(std::uint64_t))));

    // The OSR frame is responsible for deleting its input arrays as the frame that originally allocated the
    // pointer is replaced.
    llvm::FunctionCallee callee =
        function->getParent()->getOrInsertFunction("jllvm_osr_frame_delete", builder.getVoidTy(), builder.getPtrTy());
    builder.CreateCall(callee, function->getArg(0));

    std::array<llvm::Value*, 7> arguments = {methodRef,     byteCodeOffset, topOfStack,          operandStack,
                                             operandGCMask, localVariables, localVariablesGCMask};
    std::array<llvm::Type*, 7> types{};
    llvm::transform(arguments, types.begin(), std::mem_fn(&llvm::Value::getType));

    // Deopt all values used as context during interpretation. This makes it possible for the unwinder to read the
    // method, local variables, the operand stack, the bytecode offset and where GC pointers are contained during
    // unwinding.
    llvm::CallInst* callInst = builder.CreateCall(
        module->getOrInsertFunction("jllvm_interpreter",
                                    llvm::FunctionType::get(builder.getInt64Ty(), types, /*isVarArg=*/false)),
        arguments, llvm::OperandBundleDef("deopt", arguments));
    if (returnType == BaseType(BaseType::Void))
    {
        callInst = nullptr;
    }
    emitReturn(builder, callInst, callingConvention);

    debugInfoBuilder.finalize();

    Runtime& runtime = m_virtualMachine.getRuntime();
    llvm::cantFail(runtime.getLLVMIRLayer().add(m_interpreterCCSymbols,
                                                llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    return reinterpret_cast<void*>(
        llvm::cantFail(runtime.getSession().lookup({&m_interpreterCCSymbols}, runtime.getInterner()(functionName)))
            .getAddress());
}

namespace
{

/// Tag returned when interpreting an instruction to jump to a new bytecode offset.
struct SetPC
{
    std::uint16_t newPC;
};

/// Tag returned when interpreting an instruction to continue to the next instruction in the bytecode.
struct NextPC
{
};

/// Tag returned when interpreting an instruction to stop interpretation and return a result.
struct ReturnValue
{
    std::uint64_t value;

    template <jllvm::InterpreterValue T>
    ReturnValue(T value) : value(llvm::bit_cast<jllvm::NextSizedUInt<T>>(value))
    {
    }
};

using InstructionResult = swl::variant<SetPC, NextPC, ReturnValue>;

} // namespace

jllvm::ClassObject* jllvm::Interpreter::getClassObject(const ClassFile& classFile, PoolIndex<ClassInfo> index)
{
    return getClassObject(classFile, *index.resolve(classFile));
}

jllvm::ClassObject* jllvm::Interpreter::getClassObject(const ClassFile& classFile, ClassInfo classInfo)
{
    llvm::StringRef className = classInfo.nameIndex.resolve(classFile)->text;
    return &m_virtualMachine.getClassLoader().forName(FieldType::fromMangled(className));
}

std::tuple<jllvm::ClassObject*, llvm::StringRef, jllvm::FieldType>
    jllvm::Interpreter::getFieldInfo(const ClassFile& classFile, PoolIndex<FieldRefInfo> index)
{
    const FieldRefInfo* refInfo = index.resolve(classFile);
    const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(classFile);
    llvm::StringRef fieldName = nameAndTypeInfo->nameIndex.resolve(classFile)->text;
    FieldType descriptor(nameAndTypeInfo->descriptorIndex.resolve(classFile)->text);

    ClassObject* classObject = getClassObject(classFile, refInfo->classIndex);
    return {classObject, fieldName, descriptor};
}

void jllvm::Interpreter::escapeToJIT()
{
    m_virtualMachine.unwindJavaStack(
        [&](JavaFrame frame)
        {
            m_virtualMachine.getRuntime().doOnStackReplacement(
                frame,
                m_virtualMachine.getJIT().createOSRStateFromInterpreterFrame(llvm::cast<InterpreterFrame>(frame)));
        });
    llvm_unreachable("not possible");
}

namespace
{

using namespace jllvm;

/// Mapping of instruction 'T' to the type it operates on.
template <class T>
struct InstructionElementType;

template <OperatesOnReferences T>
struct InstructionElementType<T>
{
    using type = ObjectInterface*;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = type;
};

template <OperatesOnIntegers T>
struct InstructionElementType<T>
{
    using type = std::uint32_t;
    using unsigned_type = type;
    using signed_type = std::int32_t;
    using field_type = signed_type;
};

template <OperatesOnFloat T>
struct InstructionElementType<T>
{
    using type = float;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = signed_type;
};

template <OperatesOnLong T>
struct InstructionElementType<T>
{
    using type = std::uint64_t;
    using unsigned_type = type;
    using signed_type = std::int64_t;
    using field_type = signed_type;
};

template <OperatesOnDouble T>
struct InstructionElementType<T>
{
    using type = double;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = signed_type;
};

template <OperatesOnByte T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::uint8_t;
};

template <OperatesOnShort T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::int16_t;
};

template <OperatesOnChar T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::uint16_t;
};

/// Function object implementing the comparison operator performed by the instruction 'T'.
template <class T>
struct ComparisonOperator;

template <DoesEqual T>
struct ComparisonOperator<T> : std::equal_to<>
{
};

template <DoesNotEqual T>
struct ComparisonOperator<T> : std::not_equal_to<>
{
};

template <DoesLessThan T>
struct ComparisonOperator<T> : std::less<>
{
};

template <DoesGreaterEqual T>
struct ComparisonOperator<T> : std::greater_equal<>
{
};

template <DoesGreaterThan T>
struct ComparisonOperator<T> : std::greater<>
{
};

template <DoesLessEqual T>
struct ComparisonOperator<T> : std::less_equal<>
{
};

/// Maps the conversion instruction 'T' to the source, target and target type on the stack that the instruction
/// converts between.
template <DoesConversion T>
struct ConversionOperator;

template <ConvertsToFloat T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = float;
    using stack_type = target_type;
};

template <ConvertsToDouble T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = double;
    using stack_type = target_type;
};

template <ConvertsToInt T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = std::int32_t;
    using stack_type = target_type;
};

template <ConvertsToLong T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = std::int64_t;
    using stack_type = target_type;
};

template <>
struct ConversionOperator<I2B>
{
    using source_type = std::int32_t;
    using target_type = std::int8_t;
    using stack_type = source_type;
};

template <>
struct ConversionOperator<I2C>
{
    using source_type = std::int32_t;
    using target_type = std::uint16_t;
    using stack_type = source_type;
};

template <>
struct ConversionOperator<I2S>
{
    using source_type = std::int32_t;
    using target_type = std::int16_t;
    using stack_type = source_type;
};

/// Struct used to implement instructions with generic implementations parameterized on their operand types.
struct MultiTypeImpls
{
    VirtualMachine& virtualMachine;
    InterpreterContext& context;

    template <IsAdd T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs + rhs);
        return {};
    }

    template <IsSub T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs - rhs);
        return {};
    }

    template <IsNeg T>
    NextPC operator()(T) const
    {
        context.push(-context.pop<typename InstructionElementType<T>::unsigned_type>());
        return {};
    }

    template <IsMul T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs * rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, FDiv, DDiv>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::type>();
        auto lhs = context.pop<typename InstructionElementType<T>::type>();
        context.push(lhs / rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, IDiv, LDiv>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::signed_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::signed_type>();
        if (rhs == 0)
        {
            // TODO: Throw ArithmeticException.
            llvm::report_fatal_error("Throwing ArithmeticException is not yet implemented");
        }
        context.push(lhs / rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, FRem, DRem>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::type>();
        auto lhs = context.pop<typename InstructionElementType<T>::type>();
        context.push(std::fmod(lhs, rhs));
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, IRem, LRem>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::signed_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::signed_type>();
        if (rhs == 0)
        {
            // TODO: Throw ArithmeticException.
            llvm::report_fatal_error("Throwing ArithmeticException is not yet implemented");
        }
        context.push(lhs % rhs);
        return {};
    }

    template <IsOr T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs | rhs);
        return {};
    }

    template <IsAnd T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs & rhs);
        return {};
    }

    template <IsXor T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs ^ rhs);
        return {};
    }

    template <IsShl T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::unsigned_type;
        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<type>::digits - 1;
        context.push(lhs << (rhs & mask));
        return {};
    }

    template <IsShr T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::signed_type;

        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<typename InstructionElementType<T>::unsigned_type>::digits - 1;
        context.push(lhs >> (rhs & mask));
        return {};
    }

    template <IsUShr T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::unsigned_type;

        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<type>::digits - 1;
        context.push(lhs >> (rhs & mask));
        return {};
    }

    template <IsIfCmp T>
    InstructionResult operator()(T instruction) const
    {
        auto value2 = context.pop<typename InstructionElementType<T>::signed_type>();
        auto value1 = context.pop<typename InstructionElementType<T>::signed_type>();
        if (ComparisonOperator<T>{}(value1, value2))
        {
            return SetPC{static_cast<std::uint16_t>(instruction.offset + instruction.target)};
        }
        return NextPC{};
    }

    template <IsIf T>
    InstructionResult operator()(T instruction) const
    {
        auto value = context.pop<typename InstructionElementType<T>::signed_type>();
        // NOLINTNEXTLINE(*-use-nullptr): clang-tidy warns the use of '0' rather than 'nullptr' despite being templated.
        if (ComparisonOperator<T>{}(value, static_cast<decltype(value)>(0)))
        {
            return SetPC{static_cast<std::uint16_t>(instruction.offset + instruction.target)};
        }
        return NextPC{};
    }

    template <DoesConversion T>
    NextPC operator()(T) const
    {
        using source_type = typename ConversionOperator<T>::source_type;
        using target_type = typename ConversionOperator<T>::target_type;
        using stack_type = typename ConversionOperator<T>::stack_type;

        auto value = context.pop<source_type>();
        if constexpr (!std::is_floating_point_v<source_type> || !std::is_integral_v<target_type>)
        {
            // C++s builtin conversions implement the semantics required if not converting from a float to an integer.
            context.push<stack_type>(static_cast<target_type>(value));
        }
        else
        {
            auto trunc = std::trunc(value);
            if (std::isnan(trunc))
            {
                // NaNs convert to 0.
                context.push<stack_type>(0);
            }
            else if (trunc >= static_cast<source_type>(std::numeric_limits<target_type>::min())
                     && trunc <= static_cast<source_type>(std::numeric_limits<target_type>::max()))
            {
                // If after rounding the value fits within the target type, use it as is.
                context.push<stack_type>(static_cast<target_type>(trunc));
            }
            else if (trunc < 0)
            {
                // Otherwise, the float maps to either the largest or smallest integer value.
                context.push<stack_type>(std::numeric_limits<target_type>::min());
            }
            else
            {
                context.push<stack_type>(std::numeric_limits<target_type>::max());
            }
        }

        return NextPC{};
    }

    template <IsCmp T>
    NextPC operator()(T) const
    {
        auto value2 = context.pop<typename InstructionElementType<T>::signed_type>();
        auto value1 = context.pop<typename InstructionElementType<T>::signed_type>();
        if (value1 > value2)
        {
            context.push<std::int32_t>(1);
        }
        else if (value1 == value2)
        {
            context.push<std::int32_t>(0);
        }
        else if (value1 < value2)
        {
            context.push<std::int32_t>(-1);
        }
        else
        {
            // At least one of the operands is a NaN leading to all comparisons to yield false. Depending on the
            // instruction, either 1 or -1 is pushed.
            if constexpr (llvm::is_one_of<T, FCmpG, DCmpG>{})
            {
                context.push<std::int32_t>(1);
            }
            else
            {
                context.push<std::int32_t>(-1);
            }
        }
        return {};
    }

    template <IsLoad T>
    NextPC operator()(T load) const
    {
        context.pushAsRaw<typename InstructionElementType<T>::type>(context.getLocalRaw(load.index));
        return {};
    }

    template <IsLoad0 T>
    NextPC operator()(T) const
    {
        context.pushAsRaw<typename InstructionElementType<T>::type>(context.getLocalRaw(0));
        return {};
    }

    template <IsLoad1 T>
    NextPC operator()(T) const
    {
        context.pushAsRaw<typename InstructionElementType<T>::type>(context.getLocalRaw(1));
        return {};
    }

    template <IsLoad2 T>
    NextPC operator()(T) const
    {
        context.pushAsRaw<typename InstructionElementType<T>::type>(context.getLocalRaw(2));
        return {};
    }

    template <IsLoad3 T>
    NextPC operator()(T) const
    {
        context.pushAsRaw<typename InstructionElementType<T>::type>(context.getLocalRaw(3));
        return {};
    }

    template <IsALoad T>
    InstructionResult operator()(T) const
    {
        auto index = context.pop<std::int32_t>();
        auto* array = context.pop<Array<typename InstructionElementType<T>::field_type>*>();
        if (!array)
        {
            virtualMachine.throwNullPointerException();
        }
        if (index < 0 || index >= array->size())
        {
            virtualMachine.throwArrayIndexOutOfBoundsException(index, array->size());
        }
        context.push<typename InstructionElementType<T>::type>((*array)[index]);
        return NextPC{};
    }

    template <IsStore T>
    NextPC operator()(T store) const
    {
        using type = typename InstructionElementType<T>::type;
        context.setLocalAsRaw<type>(store.index, context.popAsRaw<type>());
        return {};
    }

    template <IsStore0 T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::type;
        context.setLocalAsRaw<type>(0, context.popAsRaw<type>());
        return {};
    }

    template <IsStore1 T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::type;
        context.setLocalAsRaw<type>(1, context.popAsRaw<type>());
        return {};
    }

    template <IsStore2 T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::type;
        context.setLocalAsRaw<type>(2, context.popAsRaw<type>());
        return {};
    }

    template <IsStore3 T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::type;
        context.setLocalAsRaw<type>(3, context.popAsRaw<type>());
        return {};
    }

    template <IsConst0 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(0);
        return {};
    }

    template <IsConst1 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(1);
        return {};
    }

    template <IsConst2 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(2);
        return {};
    }

    /// Implementation for all return instructions that return a value with the exception of 'ireturn'.
    /// 'ireturn' has special semantics if the return type of the method is an integer type other than 'int'.
    template <IsReturnValue T>
    ReturnValue operator()(T) const requires(!std::same_as<T, IReturn>)
    {
        return ReturnValue(context.pop<typename InstructionElementType<T>::type>());
    }

    template <IsAStore T>
    NextPC operator()(T) const
    {
        auto value = context.pop<typename InstructionElementType<T>::type>();
        auto index = context.pop<std::int32_t>();
        auto* array = context.pop<Array<typename InstructionElementType<T>::field_type>*>();
        if (!array)
        {
            virtualMachine.throwNullPointerException();
        }
        if (index < 0 || index >= array->size())
        {
            virtualMachine.throwArrayIndexOutOfBoundsException(index, array->size());
        }
        (*array)[index] = value;
        return {};
    }
};

} // namespace

std::uint64_t jllvm::Interpreter::executeMethod(const Method& method, std::uint16_t& offset,
                                                InterpreterContext& context)
{
    const ClassFile& classFile = *method.getClassObject()->getClassFile();
    Code* code = method.getMethodInfo().getAttributes().find<Code>();
    assert(code && "method being interpreted must have code");
    llvm::ArrayRef<char> codeArray = code->getCode();
    auto curr = ByteCodeIterator(codeArray.data(), offset);
    MethodType methodType = method.getType();
    std::uint64_t backEdgeCounter = 0;

    // Lazily fetches and caches the class object for 'Object'.
    auto getObjectClass = [&, objectClass = static_cast<ClassObject*>(nullptr)]() mutable
    {
        if (!objectClass)
        {
            objectClass = &m_virtualMachine.getClassLoader().forName("Ljava/lang/Object;");
        }
        return objectClass;
    };

    while (true)
    {
        // Update the current offset to the new instruction.
        offset = curr.getOffset();
        ByteCodeOp operation = *curr;
        InstructionResult result = match(
            operation, MultiTypeImpls{m_virtualMachine, context},
            [&](AConstNull)
            {
                context.push<ObjectInterface*>(nullptr);
                return NextPC{};
            },
            [&](ANewArray aNewArray)
            {
                auto count = context.pop<std::int32_t>();
                if (count < 0)
                {
                    m_virtualMachine.throwNegativeArraySizeException(count);
                }
                ClassObject* componentType = getClassObject(classFile, aNewArray.index);
                ClassObject& arrayType =
                    m_virtualMachine.getClassLoader().forName(ArrayType(componentType->getDescriptor()));
                auto* array = m_virtualMachine.getGC().allocate<AbstractArray>(&arrayType, count);
                context.push(array);
                return NextPC{};
            },
            [&](ArrayLength)
            {
                auto* array = context.pop<AbstractArray*>();
                if (!array)
                {
                    m_virtualMachine.throwNullPointerException();
                }
                context.push<std::uint32_t>(array->size());
                return NextPC{};
            },
            [&](AThrow) -> InstructionResult
            {
                auto* exception = context.pop<ObjectInterface*>();
                if (!exception)
                {
                    m_virtualMachine.throwNullPointerException();
                }
                // Verifier checks that the exception is an instance of 'Throwable' rather than performing it at
                // runtime.
                m_virtualMachine.throwJavaException(static_cast<Throwable*>(exception));
            },
            [&](BIPush biPush)
            {
                context.push<std::int32_t>(biPush.value);
                return NextPC{};
            },
            [&](CheckCast checkCast)
            {
                auto* object = context.pop<ObjectInterface*>();
                context.push(object);
                if (!object)
                {
                    return NextPC{};
                }

                ClassObject* classObject = getClassObject(classFile, checkCast.index);
                if (object->instanceOf(classObject))
                {
                    return NextPC{};
                }

                m_virtualMachine.throwClassCastException(object, classObject);
            },
            [&](Dup)
            {
                InterpreterContext::RawValue value = context.popRaw();
                context.pushRaw(value);
                context.pushRaw(value);
                return NextPC{};
            },
            [&](DupX1)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](DupX2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2X1)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2X2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                InterpreterContext::RawValue value4 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value4);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](GetField getField)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getField.index);

                const Field* field = classObject->getInstanceField(fieldName, descriptor);
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    m_virtualMachine.throwNullPointerException();
                }

                std::uint64_t value{};
                std::memcpy(&value, reinterpret_cast<char*>(object) + field->getOffset(), descriptor.sizeOf());
                context.push(value, descriptor);
                return NextPC{};
            },
            [&](GetStatic getStatic)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getStatic.index);

                m_virtualMachine.initialize(*classObject);
                const Field* field = classObject->getStaticField(fieldName, descriptor);

                std::uint64_t value{};
                std::memcpy(&value, field->getAddressOfStatic(), descriptor.sizeOf());
                context.push(value, descriptor);
                return NextPC{};
            },
            [&](OneOf<Goto, GotoW> gotoInst)
            { return SetPC{static_cast<std::uint16_t>(gotoInst.offset + gotoInst.target)}; },
            [&](IConst3)
            {
                context.push<std::int32_t>(3);
                return NextPC{};
            },
            [&](IConst4)
            {
                context.push<std::int32_t>(4);
                return NextPC{};
            },
            [&](IConst5)
            {
                context.push<std::int32_t>(5);
                return NextPC{};
            },
            [&](IConstM1)
            {
                context.push<std::int32_t>(-1);
                return NextPC{};
            },
            [&](IInc iInc)
            {
                context.setLocal(iInc.index,
                                 static_cast<std::int32_t>(iInc.byte) + context.getLocal<std::uint32_t>(iInc.index));
                return NextPC{};
            },
            [&](InstanceOf instanceOf)
            {
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    context.push<std::int32_t>(0);
                    return NextPC{};
                }

                ClassObject* classObject = getClassObject(classFile, instanceOf.index);
                context.push<std::int32_t>(object->instanceOf(classObject));
                return NextPC{};
            },
            // TODO: InvokeDynamic
            [&](OneOf<InvokeStatic, InvokeSpecial, InvokeInterface, InvokeVirtual> invoke)
            {
                const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(classFile);

                llvm::StringRef methodName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                MethodType descriptor(
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

                // Initialize the class object if it's an 'invokestatic'. This has to be done before the call to
                // 'viewAndPopArguments' as the arguments on the operand stack could otherwise be garbage collected.
                ClassObject* classObject = getClassObject(classFile, refInfo->classIndex);
                if (holds_alternative<InvokeStatic>(operation))
                {
                    m_virtualMachine.initialize(*classObject);
                }

                llvm::ArrayRef<std::uint64_t> arguments =
                    context.viewAndPopArguments(descriptor, /*isStatic=*/holds_alternative<InvokeStatic>(operation));

                // Find the callee with the resolution of the given call.
                const Method* callee = match(
                    operation,
                    [&](InvokeStatic) -> const Method*
                    {
                        return classObject->isInterface() ?
                                   classObject->interfaceMethodResolution(methodName, descriptor, getObjectClass()) :
                                   classObject->methodResolution(methodName, descriptor);
                    },
                    [&](OneOf<InvokeInterface, InvokeVirtual>) -> const Method*
                    {
                        auto* thisArg = llvm::bit_cast<ObjectInterface*>(arguments.front());
                        if (!thisArg)
                        {
                            m_virtualMachine.throwNullPointerException();
                        }

                        // TODO: This is super unoptimized. V-Table and I-Table lookups could be introduced just for
                        //       the interpreter, and inline-caching used.
                        const Method* resolvedMethod;
                        if (holds_alternative<InvokeVirtual>(operation))
                        {
                            resolvedMethod = classObject->methodResolution(methodName, descriptor);
                        }
                        else
                        {
                            resolvedMethod =
                                classObject->interfaceMethodResolution(methodName, descriptor, getObjectClass());
                        }

                        // Fast path: If its known that the method has no table slot due to not being overridable, we
                        // do not have to perform method selection.
                        if (!resolvedMethod->getTableSlot())
                        {
                            return resolvedMethod;
                        }

                        // Select the correct method based on the dynamic type of the 'this' argument.
                        return &thisArg->getClass()->methodSelection(*resolvedMethod);
                    },
                    [&](InvokeSpecial)
                    {
                        auto* thisArg = llvm::bit_cast<ObjectInterface*>(arguments.front());
                        if (!thisArg)
                        {
                            m_virtualMachine.throwNullPointerException();
                        }

                        return classObject->specialMethodResolution(methodName, descriptor, getObjectClass(),
                                                                    method.getClassObject());
                    },
                    [&](...) -> const Method* { llvm_unreachable("unexpected op"); });

                std::uint64_t returnValue = callee->callInterpreterCC(arguments.data());
                FieldType returnType = descriptor.returnType();
                if (returnType != BaseType(BaseType::Void))
                {
                    context.push(returnValue, returnType);
                }

                return NextPC{};
            },
            [&](IReturn)
            {
                auto value = context.pop<std::uint32_t>();
                switch (get<BaseType>(methodType.returnType()).getValue())
                {
                    case BaseType::Boolean: value &= 0b1; break;
                    case BaseType::Char: value = static_cast<std::int32_t>(static_cast<std::uint16_t>(value)); break;
                    case BaseType::Byte: value = static_cast<std::int32_t>(static_cast<std::int8_t>(value)); break;
                    case BaseType::Short: value = static_cast<std::int32_t>(static_cast<std::int16_t>(value)); break;
                    case BaseType::Int: break;
                    case BaseType::Long:
                    case BaseType::Void:
                    case BaseType::Float:
                    case BaseType::Double:
                    default: llvm_unreachable("not possible");
                }
                return ReturnValue(value);
            },
            [&](OneOf<JSR, JSRw> jsr)
            {
                std::uint16_t retAddress =
                    jsr.offset + sizeof(OpCodes)
                    + (holds_alternative<JSRw>(*curr) ? sizeof(std::int32_t) : sizeof(std::int16_t));
                context.pushRaw(retAddress, /*isReference=*/false);
                return SetPC{static_cast<std::uint16_t>(jsr.offset + jsr.target)};
            },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(classFile), [&](const IntegerInfo* integerInfo) { context.push(integerInfo->value); },
                    [&](const FloatInfo* floatInfo) { context.push(floatInfo->value); },
                    [&](const LongInfo* longInfo) { context.push(longInfo->value); },
                    [&](const DoubleInfo* doubleInfo) { context.push(doubleInfo->value); },
                    [&](const ClassInfo* classInfo) { context.push(getClassObject(classFile, *classInfo)); },
                    [&](const StringInfo* stringInfo)
                    {
                        llvm::StringRef utf8String = stringInfo->stringValue.resolve(classFile)->text;
                        context.push(m_virtualMachine.getStringInterner().intern(utf8String));
                    },
                    [&](const auto*) { escapeToJIT(); });
                return NextPC{};
            },
            [&](const LookupSwitch& switchOp)
            {
                auto index = context.pop<std::int32_t>();
                auto result =
                    llvm::lower_bound(switchOp.matchOffsetPairs(), index,
                                      [](const auto& pair, std::int32_t value) { return pair.first < value; });
                if (result == switchOp.matchOffsetPairs().end() || (*result).first != index)
                {
                    return SetPC{static_cast<std::uint16_t>(switchOp.offset + switchOp.defaultOffset)};
                }
                return SetPC{static_cast<std::uint16_t>(switchOp.offset + (*result).second)};
            },
            [&](OneOf<MonitorEnter, MonitorExit>)
            {
                // Pop object as is required by the instruction.
                // TODO: If we ever care about multi threading, this would require lazily creating a mutex and
                //  (un)locking it.
                if (!context.pop<ObjectInterface*>())
                {
                    m_virtualMachine.throwNullPointerException();
                }
                return NextPC{};
            },
            [&](MultiANewArray multiANewArray)
            {
                GarbageCollector& gc = m_virtualMachine.getGC();
                ClassObject* classObject = getClassObject(classFile, multiANewArray.index);
                std::vector<std::int32_t> counts(multiANewArray.dimensions);

                std::generate(counts.rbegin(), counts.rend(), [&] { return context.pop<std::int32_t>(); });

                for (std::int32_t count : counts)
                {
                    if (count < 0)
                    {
                        m_virtualMachine.throwNegativeArraySizeException(count);
                    }
                };

                auto generateArray = [&](llvm::ArrayRef<std::int32_t> counts, ArrayType currentType,
                                         const auto generator) -> ObjectInterface*
                {
                    std::int32_t count = counts.front();
                    counts = counts.drop_front();
                    ClassObject& arrayType = m_virtualMachine.getClassLoader().forName(currentType);
                    GCUniqueRoot array = gc.root(gc.allocate<AbstractArray>(&arrayType, count));
                    if (!counts.empty())
                    {
                        auto outerArray = static_cast<GCRootRef<Array<>>>(array);
                        auto componentType = get<ArrayType>(currentType.getComponentType());
                        // necessary, because iterator for Arrays is not gc safe
                        for (std::uint32_t i : llvm::seq(0u, outerArray->size()))
                        {
                            // allocation must happen before indexing
                            ObjectInterface* innerArray = generator(counts, componentType, generator);
                            (*outerArray)[i] = innerArray;
                        }
                    }
                    return array;
                };

                context.push(generateArray(counts, get<ArrayType>(classObject->getDescriptor()), generateArray));

                return NextPC{};
            },
            [&](New newInst)
            {
                ClassObject* classObject = getClassObject(classFile, newInst.index);
                m_virtualMachine.initialize(*classObject);
                context.push(m_virtualMachine.getGC().allocate(classObject));
                return NextPC{};
            },
            [&](NewArray newArray)
            {
                auto count = context.pop<std::int32_t>();
                if (count < 0)
                {
                    m_virtualMachine.throwNegativeArraySizeException(count);
                }

                ClassObject& arrayType =
                    m_virtualMachine.getClassLoader().forName(ArrayType{BaseType{newArray.componentType}});
                auto* array = m_virtualMachine.getGC().allocate<AbstractArray>(&arrayType, count);
                context.push(array);
                return NextPC{};
            },
            [&](Nop) { return NextPC{}; },
            [&](Pop)
            {
                context.popRaw();
                return NextPC{};
            },
            [&](Pop2)
            {
                context.popRaw();
                context.popRaw();
                return NextPC{};
            },
            [&](PutField putField)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, putField.index);
                const Field* field = classObject->getInstanceField(fieldName, descriptor);

                std::uint64_t value = context.pop(descriptor);
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    m_virtualMachine.throwNullPointerException();
                }

                std::memcpy(reinterpret_cast<char*>(object) + field->getOffset(), &value, descriptor.sizeOf());
                return NextPC{};
            },
            [&](PutStatic getStatic)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getStatic.index);

                m_virtualMachine.initialize(*classObject);
                Field* field = classObject->getStaticField(fieldName, descriptor);

                std::uint64_t value = context.pop(descriptor);
                std::memcpy(field->getAddressOfStatic(), &value, descriptor.sizeOf());
                return NextPC{};
            },
            [&](Ret ret)
            {
                std::uint16_t retAddress = context.getLocalRaw(ret.index).first;
                return SetPC{retAddress};
            },
            [&](Return)
            {
                // "Noop" return value for void methods.
                return ReturnValue{0};
            },
            [&](SIPush siPush)
            {
                context.push<std::int32_t>(siPush.value);
                return NextPC{};
            },
            [&](Swap)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value2);
                return NextPC{};
            },
            [&](const TableSwitch& tableSwitch)
            {
                auto index = context.pop<std::int32_t>();
                if (index < tableSwitch.low || (index - tableSwitch.low) >= tableSwitch.jumpTable.size())
                {
                    return SetPC{static_cast<std::uint16_t>(tableSwitch.offset + tableSwitch.defaultOffset)};
                }
                return SetPC{
                    static_cast<std::uint16_t>(tableSwitch.offset + tableSwitch.jumpTable[index - tableSwitch.low])};
            },
            [&](Wide wide) -> InstructionResult
            {
#define WIDE_LOAD_CASE(op)                                                            \
    case OpCodes::op:                                                                 \
    {                                                                                 \
        context.push(context.getLocal<InstructionElementType<op>::type>(wide.index)); \
        break;                                                                        \
    }

#define WIDE_STORE_CASE(op)                                                            \
    case OpCodes::op:                                                                  \
    {                                                                                  \
        context.setLocal(wide.index, context.pop<InstructionElementType<op>::type>()); \
        break;                                                                         \
    }
                switch (wide.opCode)
                {
                    WIDE_LOAD_CASE(ALoad)
                    WIDE_LOAD_CASE(DLoad)
                    WIDE_LOAD_CASE(FLoad)
                    WIDE_LOAD_CASE(ILoad)
                    WIDE_LOAD_CASE(LLoad)
                    WIDE_STORE_CASE(AStore)
                    WIDE_STORE_CASE(DStore)
                    WIDE_STORE_CASE(FStore)
                    WIDE_STORE_CASE(IStore)
                    WIDE_STORE_CASE(LStore)
                    case OpCodes::Ret:
                    {
                        std::uint16_t retAddress = context.getLocalRaw(wide.index).first;
                        return SetPC{retAddress};
                    }
                    case OpCodes::IInc:
                    {
                        context.setLocal(wide.index, static_cast<std::int32_t>(*wide.value)
                                                         + context.getLocal<std::uint32_t>(wide.index));
                        break;
                    }
                    default: llvm_unreachable("Invalid wide operation");
                }
#undef WIDE_LOAD_CASE
#undef WIDE_STORE_CASE

                return NextPC{};
            },
            [&](...) -> InstructionResult
            {
                // TODO: Remove this once the interpreter implements all opcodes.
                llvm_unreachable("NOT YET IMPLEMENTED");
            });

        if (auto* returnValue = get_if<ReturnValue>(&result))
        {
            return returnValue->value;
        }

        match(
            result, [](ReturnValue) {}, [&](NextPC) { ++curr; },
            [&](SetPC setPc)
            {
                // Backedge.
                if (setPc.newPC < offset)
                {
                    backEdgeCounter++;
                    if (backEdgeCounter == m_backEdgeThreshold)
                    {
                        escapeToJIT();
                    }
                }
                curr = ByteCodeIterator(codeArray.data(), setPc.newPC);
            });
    }
}

void* Interpreter::getOSREntry(const Method& method, std::uint16_t /*byteCodeOffset*/,
                               CallingConvention callingConvention)
{
    if (callingConvention == CallingConvention::Interpreter)
    {
        return m_interpreterInterpreterCCOSREntry;
    }
    FieldType type = method.getType().returnType();
    if (type.isReference())
    {
        return m_interpreterJITCCOSREntryReferenceReturn;
    }
    return m_interpreterJITCCOSREntries[get<BaseType>(type).getValue() - BaseType::MinValue];
}

OSRState Interpreter::createOSRStateFromInterpreterFrame(InterpreterFrame frame)
{
    return OSRState(*this, *frame.getByteCodeOffset(),
                    createOSRBuffer(*frame.getMethod(), *frame.getByteCodeOffset(), frame.readLocals(),
                                    frame.getOperandStack(), frame.getLocalsGCMask(), frame.getOperandStackGCMask()));
}

OSRState Interpreter::createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                                        Throwable* throwable)
{
    llvm::SmallVector<std::uint64_t> localsGcMask = frame.readLocalsGCMask();
    auto operandStackGCMask = std::initializer_list<std::uint64_t>{0b1};
    return OSRState(
        *this, handlerOffset,
        createOSRBuffer(
            *frame.getMethod(), handlerOffset, frame.readLocals(),
            /*operandStack=*/std::initializer_list<std::uint64_t>{reinterpret_cast<std::uint64_t>(throwable)},
            BitArrayRef(localsGcMask.data(), localsGcMask.size() * 64),
            BitArrayRef(data(operandStackGCMask), operandStackGCMask.size() * 64)));
}

std::unique_ptr<std::uint64_t[]> Interpreter::createOSRBuffer(const Method& method, std::uint16_t byteCodeOffset,
                                                              llvm::ArrayRef<std::uint64_t> locals,
                                                              llvm::ArrayRef<std::uint64_t> operandStack,
                                                              BitArrayRef<> localsGCMask,
                                                              BitArrayRef<> operandStackGCMask)
{
    std::size_t numLocals = llvm::size(locals);
    std::size_t numOperandStack = llvm::size(operandStack);

    auto buffer = std::make_unique<std::uint64_t[]>(2 + numLocals + numOperandStack + localsGCMask.numWords()
                                                    + operandStackGCMask.numWords());
    buffer[0] = reinterpret_cast<std::uintptr_t>(&method);
    buffer[1] = byteCodeOffset | numOperandStack << 16;

    auto* outIter = llvm::copy(locals, std::next(buffer.get(), 2));
    outIter = llvm::copy(operandStack, outIter);
    outIter = std::copy_n(localsGCMask.words_begin(), localsGCMask.numWords(), outIter);
    std::copy_n(operandStackGCMask.words_begin(), operandStackGCMask.numWords(), outIter);
    return buffer;
}

void Interpreter::add(const Method& method)
{
    llvm::cantFail(m_compiled2InterpreterLayer.add(m_jit2InterpreterSymbols, &method));
    // All methods in the interpreter calling convention simply reuse the single entry.
    llvm::cantFail(m_interpreterCCSymbols.define(
        llvm::orc::absoluteSymbols({{m_compiled2InterpreterLayer.getInterner()(mangleDirectMethodCall(&method)),
                                     llvm::JITEvaluatedSymbol::fromPointer(m_interpreterEntry)}})));
}
