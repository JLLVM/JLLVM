#include "CodeGenerator.hpp"

using namespace jllvm;

namespace
{
llvm::GlobalVariable* activeException(llvm::Module* module)
{
    return llvm::cast<llvm::GlobalVariable>(
        module->getOrInsertGlobal("activeException", jllvm::referenceType(module->getContext())));
}

llvm::FunctionCallee allocationFunction(llvm::Module* module)
{
    auto* function = module->getFunction("jllvm_gc_alloc");
    if (function)
    {
        return function;
    }

    function = llvm::Function::Create(llvm::FunctionType::get(referenceType(module->getContext()),
                                                              {llvm::Type::getInt32Ty(module->getContext())}, false),
                                      llvm::GlobalValue::ExternalLinkage, "jllvm_gc_alloc", module);
    function->addFnAttrs(llvm::AttrBuilder(module->getContext())
                             .addAllocSizeAttr(0, std::nullopt)
                             .addAllocKindAttr(llvm::AllocFnKind::Alloc | llvm::AllocFnKind::Zeroed));
    return function;
}

llvm::Value* extendToStackType(llvm::IRBuilder<>& builder, jllvm::FieldType& type, llvm::Value* value)
{
    return match(
        type,
        [&](BaseType baseType)
        {
            switch (baseType.getValue())
            {
                case BaseType::Boolean:
                case BaseType::Byte:
                case BaseType::Short:
                {
                    return builder.CreateSExt(value, builder.getInt32Ty());
                }
                case BaseType::Char:
                {
                    return builder.CreateZExt(value, builder.getInt32Ty());
                    break;
                }
                default: return value;
            }
        },
        [&](const auto&) { return value; });
}

inline bool isCategoryTwo(llvm::Type* type)
{
    return type->isIntegerTy(64) || type->isDoubleTy();
}

/// Truncates 'i32' args which is the type used internally on Javas operand stack for everything but 'long'
/// to integer types of the bit-width of the callee (e.g. 'i8' for a 'byte' arg in Java).
void prepareArgumentsForCall(llvm::IRBuilder<>& builder, llvm::MutableArrayRef<llvm::Value*> args,
                             llvm::FunctionType* functionType)
{
    for (auto [arg, argType] : llvm::zip(args, functionType->params()))
    {
        if (arg->getType() == argType)
        {
            continue;
        }
        assert(arg->getType()->isIntegerTy() && argType->isIntegerTy()
               && arg->getType()->getIntegerBitWidth() > argType->getIntegerBitWidth());
        arg = builder.CreateTrunc(arg, argType);
    }
}

struct ArrayInfo
{
    llvm::StringRef descriptor;
    llvm::Type* type{};
    std::size_t size{};
    std::size_t elementOffset{};
};

ArrayInfo resolveNewArrayInfo(ArrayOp::ArrayType arrayType, llvm::IRBuilder<>& builder)
{
    switch (arrayType)
    {
        case ArrayOp::ArrayType::TBoolean:
            return {"Z", builder.getInt8Ty(), sizeof(std::uint8_t), jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TChar:
            return {"C", builder.getInt16Ty(), sizeof(std::uint16_t),
                    jllvm::Array<std::uint16_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TFloat:
            return {"F", builder.getFloatTy(), sizeof(float), jllvm::Array<float>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TDouble:
            return {"D", builder.getDoubleTy(), sizeof(double), jllvm::Array<double>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TByte:
            return {"B", builder.getInt8Ty(), sizeof(std::uint8_t), jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TShort:
            return {"S", builder.getInt16Ty(), sizeof(std::int16_t), jllvm::Array<std::int16_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TInt:
            return {"I", builder.getInt32Ty(), sizeof(std::int32_t), jllvm::Array<std::int32_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TLong:
            return {"J", builder.getInt64Ty(), sizeof(std::int64_t), jllvm::Array<std::int64_t>::arrayElementsOffset()};
        default: llvm_unreachable("Invalid array type");
    }
}

} // namespace

void CodeGenerator::generateCode(const Code& code)
{
    // We need pointer size bytes, since that is the largest type we may store in a local.
    std::generate(locals.begin(), locals.end(), [&] { return builder.CreateAlloca(builder.getPtrTy()); });

    // Arguments are put into the locals. According to the specification, i64s and doubles are split into two
    // locals. We don't actually do that, we just put them into the very first local, but we still have to skip over
    // the following local as if we didn't.
    auto nextLocal = locals.begin();
    for (auto& arg : m_function->args())
    {
        builder.CreateStore(&arg, *nextLocal++);
        if (arg.getType()->isIntegerTy(64) || arg.getType()->isDoubleTy())
        {
            nextLocal++;
        }
    }

    calculateBasicBlocks(code);
    generateCodeBody(code);
}

void CodeGenerator::calculateBasicBlocks(const Code& code)
{
    for (ByteCodeOp operation : byteCodeRange(code.getCode()))
    {
        auto addBasicBlock = [&](std::uint16_t target)
        {
            auto [result, inserted] = basicBlocks.insert({target, nullptr});

            if (inserted)
            {
                result->second = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
            }
        };
        match(
            operation, [&](OneOf<Goto, GotoW> gotoOp) { addBasicBlock(gotoOp.target + gotoOp.offset); },
            [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                      IfGe, IfGt, IfLe, IfNonNull, IfNull>
                    cmpOp)
            {
                addBasicBlock(cmpOp.target + cmpOp.offset);
                addBasicBlock(cmpOp.offset + sizeof(OpCodes) + sizeof(std::int16_t));
            },
            [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
            {
                addBasicBlock(switchOp.offset + switchOp.defaultOffset);

                for (std::int32_t target : llvm::make_second_range(switchOp.matchOffsetsPairs))
                {
                    addBasicBlock(switchOp.offset + target);
                }
            },
            [](...) {});
    }

    for (const auto& iter : code.getExceptionTable())
    {
        auto [result, inserted] = basicBlocks.insert({iter.handlerPc, nullptr});
        if (!inserted)
        {
            continue;
        }
        // Handlers have the special semantic of only having the caught exception at the very top. It is therefore
        // required that we register that fact in 'basicBlockStackStates' explicitly.
        result->second = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
        basicBlockStackStates.insert({result->second, operandStack.getHandlerState()});
    }
}

void CodeGenerator::generateCodeBody(const Code& code)
{
    llvm::DenseMap<std::uint16_t, std::vector<Code::ExceptionTable>> startHandlers;
    for (const auto& iter : code.getExceptionTable())
    {
        startHandlers[iter.startPc].push_back(iter);
    }

    llvm::DenseMap<std::uint16_t, std::vector<std::list<HandlerInfo>::iterator>> endHandlers;
    for (ByteCodeOp operation : byteCodeRange(code.getCode()))
    {
        std::size_t offset = getOffset(operation);
        if (auto result = endHandlers.find(offset); result != endHandlers.end())
        {
            for (auto iter : result->second)
            {
                activeHandlers.erase(iter);
            }
            // No longer needed.
            endHandlers.erase(result);
        }

        if (auto result = startHandlers.find(offset); result != startHandlers.end())
        {
            for (const Code::ExceptionTable& iter : result->second)
            {
                activeHandlers.emplace_back(iter.handlerPc, iter.catchType);
                endHandlers[iter.endPc].push_back(std::prev(activeHandlers.end()));
            }
            // No longer needed.
            startHandlers.erase(result);
        }

        if (auto result = basicBlocks.find(offset); result != basicBlocks.end())
        {
            // Without any branches, there will not be a terminator at the end of the basic block. Thus, we need to
            // set this manually to the new insert point. This essentially implements implicit fallthrough from JVM
            // bytecode.
            if (builder.GetInsertBlock()->getTerminator() == nullptr)
            {
                basicBlockStackStates.insert({result->second, operandStack.saveState()});
                builder.CreateBr(result->second);
            }
            builder.SetInsertPoint(result->second);
            if (auto resultStackPointer = basicBlockStackStates.find(result->second);
                resultStackPointer != basicBlockStackStates.end())
            {
                operandStack.restoreState(resultStackPointer->second);
            }
        }

        generateInstruction(std::move(operation));
    }
}

void CodeGenerator::generateInstruction(ByteCodeOp operation)
{
    match(
        operation, [](...) { llvm_unreachable("NOT YET IMPLEMENTED"); },
        [&](OneOf<AALoad, BALoad, CALoad, DALoad, FALoad, IALoad, LALoad, SALoad>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid array load operation"); },
                [&](AALoad) -> llvm::Type* { return referenceType(builder.getContext()); },
                [&](BALoad) { return builder.getInt8Ty(); },
                [&](OneOf<CALoad, SALoad>) { return builder.getInt16Ty(); },
                [&](DALoad) { return builder.getDoubleTy(); }, [&](FALoad) { return builder.getFloatTy(); },
                [&](IALoad) { return builder.getInt32Ty(); }, [&](LALoad) { return builder.getInt64Ty(); });

            llvm::Value* index = operandStack.pop_back();
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = operandStack.pop_back();

            // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
            llvm::Value* gep =
                builder.CreateGEP(arrayStructType(type), array, {builder.getInt32(0), builder.getInt32(2), index});
            llvm::Value* value = builder.CreateLoad(type, gep);

            match(
                operation, [](...) {},
                [&](OneOf<BALoad, SALoad>) { value = builder.CreateSExt(value, builder.getInt32Ty()); },
                [&](CALoad) { value = builder.CreateZExt(value, builder.getInt32Ty()); });

            operandStack.push_back(value);
        },
        [&](OneOf<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid array load operation"); },
                [&](AAStore) { return referenceType(builder.getContext()); },
                [&](BAStore) { return builder.getInt8Ty(); },
                [&](OneOf<CAStore, SAStore>) { return builder.getInt16Ty(); },
                [&](DAStore) { return builder.getDoubleTy(); }, [&](FAStore) { return builder.getFloatTy(); },
                [&](IAStore) { return builder.getInt32Ty(); }, [&](LAStore) { return builder.getInt64Ty(); });

            llvm::Value* value = operandStack.pop_back();
            llvm::Value* index = operandStack.pop_back();
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = operandStack.pop_back();

            // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
            llvm::Value* gep =
                builder.CreateGEP(arrayStructType(type), array, {builder.getInt32(0), builder.getInt32(2), index});
            match(
                operation, [](...) {},
                [&, arrayType = type](OneOf<BAStore, CAStore, SAStore>)
                { value = builder.CreateTrunc(value, arrayType); });

            builder.CreateStore(value, gep);
        },
        [&](AConstNull)
        { operandStack.push_back(llvm::ConstantPointerNull::get(referenceType(builder.getContext()))); },
        [&](OneOf<ALoad, DLoad, FLoad, ILoad, LLoad> load)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid load operation"); },
                [&](ALoad) { return referenceType(builder.getContext()); },
                [&](DLoad) { return builder.getDoubleTy(); }, [&](FLoad) { return builder.getFloatTy(); },
                [&](ILoad) { return builder.getInt32Ty(); }, [&](LLoad) { return builder.getInt64Ty(); });

            operandStack.push_back(builder.CreateLoad(type, locals[load.index]));
        },
        [&](OneOf<ALoad0, DLoad0, FLoad0, ILoad0, LLoad0, ALoad1, DLoad1, FLoad1, ILoad1, LLoad1, ALoad2, DLoad2,
                  FLoad2, ILoad2, LLoad2, ALoad3, DLoad3, FLoad3, ILoad3, LLoad3>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid load operation"); },
                [&](OneOf<ALoad0, ALoad1, ALoad2, ALoad3>) { return referenceType(builder.getContext()); },
                [&](OneOf<DLoad0, DLoad1, DLoad2, DLoad3>) { return builder.getDoubleTy(); },
                [&](OneOf<FLoad0, FLoad1, FLoad2, FLoad3>) { return builder.getFloatTy(); },
                [&](OneOf<ILoad0, ILoad1, ILoad2, ILoad3>) { return builder.getInt32Ty(); },
                [&](OneOf<LLoad0, LLoad1, LLoad2, LLoad3>) { return builder.getInt64Ty(); });

            auto index = match(
                operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid load operation"); },
                [&](OneOf<ALoad0, DLoad0, FLoad0, ILoad0, LLoad0>) { return 0; },
                [&](OneOf<ALoad1, DLoad1, FLoad1, ILoad1, LLoad1>) { return 1; },
                [&](OneOf<ALoad2, DLoad2, FLoad2, ILoad2, LLoad2>) { return 2; },
                [&](OneOf<ALoad3, DLoad3, FLoad3, ILoad3, LLoad3>) { return 3; });

            operandStack.push_back(builder.CreateLoad(type, locals[index]));
        },
        [&](ANewArray aNewArray)
        {
            auto index = PoolIndex<ClassInfo>{aNewArray.index};
            // TODO: throw NegativeArraySizeException
            llvm::Value* count = operandStack.pop_back();

            llvm::Value* classObject = m_helper.getClassObject(
                builder, "[L" + index.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text + ";");

            // Size required is the size of the array prior to the elements (equal to the offset to the
            // elements) plus element count * element size.
            llvm::Value* bytesNeeded = builder.getInt32(Array<>::arrayElementsOffset());
            bytesNeeded = builder.CreateAdd(bytesNeeded, builder.CreateMul(count, builder.getInt32(sizeof(Object*))));

            llvm::Value* object = builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);
            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            // Type object.
            builder.CreateStore(classObject, object);
            // Array length.
            auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), object,
                                          {builder.getInt32(0), builder.getInt32(1)});
            builder.CreateStore(count, gep);

            operandStack.push_back(object);
        },
        [&](OneOf<AReturn, DReturn, FReturn, IReturn, LReturn>)
        {
            llvm::Value* value = operandStack.pop_back();

            match(
                operation, [](...) {},
                [&](IReturn)
                {
                    if (functionMethodType.returnType == FieldType(BaseType::Boolean))
                    {
                        value = builder.CreateAnd(value, builder.getInt32(1));
                    }
                    if (m_function->getReturnType() != value->getType())
                    {
                        value = builder.CreateTrunc(value, m_function->getReturnType());
                    }
                });

            builder.CreateRet(value);
        },
        [&](ArrayLength)
        {
            llvm::Value* array = operandStack.pop_back();

            // The element type of the array type here is actually irrelevant.
            llvm::Value* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                                 {builder.getInt32(0), builder.getInt32(1)});
            operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), gep));
        },
        [&](OneOf<AStore, DStore, FStore, IStore, LStore> store)
        { builder.CreateStore(operandStack.pop_back(), locals[store.index]); },
        [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0, AStore1, DStore1, FStore1, IStore1, LStore1, AStore2,
                  DStore2, FStore2, IStore2, LStore2, AStore3, DStore3, FStore3, IStore3, LStore3>)
        {
            auto index = match(
                operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0>) { return 0; },
                [&](OneOf<AStore1, DStore1, FStore1, IStore1, LStore1>) { return 1; },
                [&](OneOf<AStore2, DStore2, FStore2, IStore2, LStore2>) { return 2; },
                [&](OneOf<AStore3, DStore3, FStore3, IStore3, LStore3>) { return 3; });

            builder.CreateStore(operandStack.pop_back(), locals[index]);
        },
        [&](AThrow)
        {
            llvm::Value* exception = operandStack.pop_back();

            builder.CreateStore(exception, activeException(m_function->getParent()));

            builder.CreateBr(generateHandlerChain(exception, builder.GetInsertBlock()));
        },
        [&](BIPush biPush)
        {
            llvm::Value* res = builder.getInt32(biPush.value);
            operandStack.push_back(res);
        },
        [&](OneOf<CheckCast, InstanceOf> op)
        {
            llvm::PointerType* ty = referenceType(builder.getContext());
            llvm::Value* object = operandStack.pop_back();
            llvm::Value* null = llvm::ConstantPointerNull::get(ty);

            llvm::Value* isNull = builder.CreateICmpEQ(object, null);
            auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
            auto* instanceOfBlock = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
            llvm::BasicBlock* block = builder.GetInsertBlock();
            builder.CreateCondBr(isNull, continueBlock, instanceOfBlock);

            builder.SetInsertPoint(instanceOfBlock);

            llvm::Value* classObject = loadClassObjectFromPool(op.index);

            llvm::FunctionCallee callee = m_function->getParent()->getOrInsertFunction(
                "jllvm_instance_of", llvm::FunctionType::get(builder.getInt32Ty(), {ty, ty}, false));
            llvm::Instruction* call = builder.CreateCall(callee, {object, classObject});

            match(
                operation, [](...) { llvm_unreachable("Invalid operation"); },
                [&](InstanceOf)
                {
                    builder.CreateBr(continueBlock);

                    builder.SetInsertPoint(continueBlock);
                    llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
                    // null references always return 0.
                    phi->addIncoming(builder.getInt32(0), block);
                    phi->addIncoming(call, call->getParent());

                    operandStack.push_back(phi);
                },
                [&](CheckCast)
                {
                    operandStack.push_back(object);
                    auto* throwBlock = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
                    builder.CreateCondBr(builder.CreateTrunc(call, builder.getInt1Ty()), continueBlock, throwBlock);

                    builder.SetInsertPoint(throwBlock);

                    llvm::Value* exception = builder.CreateCall(
                        m_function->getParent()->getOrInsertFunction("jllvm_build_class_cast_exception",
                                                                     llvm::FunctionType::get(ty, {ty, ty}, false)),
                        {object, classObject});

                    builder.CreateStore(exception, activeException(m_function->getParent()));

                    builder.CreateBr(generateHandlerChain(exception, builder.GetInsertBlock()));

                    builder.SetInsertPoint(continueBlock);
                });
        },
        [&](D2F)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateFPTrunc(value, builder.getFloatTy()));
        },
        [&](OneOf<D2I, D2L, F2I, F2L>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid conversion operation"); },
                [&](OneOf<D2I, F2I>) { return builder.getInt32Ty(); },
                [&](OneOf<D2L, F2L>) { return builder.getInt64Ty(); });

            llvm::Value* value = operandStack.pop_back();

            operandStack.push_back(builder.CreateIntrinsic(type, llvm::Intrinsic::fptosi_sat, {value}));
        },
        [&](OneOf<DAdd, FAdd, IAdd, LAdd>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            auto* sum = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid add operation"); },
                [&](OneOf<DAdd, FAdd>) { return builder.CreateFAdd(lhs, rhs); },
                [&](OneOf<IAdd, LAdd>) { return builder.CreateAdd(lhs, rhs); });

            operandStack.push_back(sum);
        },
        [&](OneOf<DCmpG, DCmpL, FCmpG, FCmpL>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            // using unordered compare to allow for NaNs
            // if lhs == rhs result is 0, otherwise the resulting boolean is converted for the default case
            llvm::Value* notEqual = builder.CreateFCmpUNE(lhs, rhs);
            llvm::Value* otherCmp;
            llvm::Value* otherCase;

            if (holds_alternative<FCmpG>(operation) || holds_alternative<DCmpG>(operation))
            {
                // is 0 if lhs == rhs, otherwise 1 for lhs > rhs or either operand being NaN
                notEqual = builder.CreateZExt(notEqual, builder.getInt32Ty());
                // using ordered less than to check lhs < rhs
                otherCmp = builder.CreateFCmpOLT(lhs, rhs);
                // return -1 if lhs < rhs
                otherCase = builder.getInt32(-1);
            }
            else
            {
                // is 0 if lhs == rhs, otherwise -1 for lhs < rhs or either operand being NaN
                notEqual = builder.CreateSExt(notEqual, builder.getInt32Ty());
                // using ordered greater than to check lhs > rhs
                otherCmp = builder.CreateFCmpOGT(lhs, rhs);
                // return -1 if lhs > rhs
                otherCase = builder.getInt32(1);
            }

            // select the non-default or the 0-or-default value based on the result of otherCmp
            operandStack.push_back(builder.CreateSelect(otherCmp, otherCase, notEqual));
        },
        [&](OneOf<DConst0, DConst1, FConst0, FConst1, FConst2, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4,
                  IConst5, LConst0, LConst1>)
        {
            auto* value = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid const operation"); },
                [&](DConst0) { return llvm::ConstantFP::get(builder.getDoubleTy(), 0.0); },
                [&](DConst1) { return llvm::ConstantFP::get(builder.getDoubleTy(), 1.0); },
                [&](FConst0) { return llvm::ConstantFP::get(builder.getFloatTy(), 0.0); },
                [&](FConst1) { return llvm::ConstantFP::get(builder.getFloatTy(), 1.0); },
                [&](FConst2) { return llvm::ConstantFP::get(builder.getFloatTy(), 2.0); },
                [&](IConstM1) { return builder.getInt32(-1); }, [&](IConst0) { return builder.getInt32(0); },
                [&](IConst1) { return builder.getInt32(1); }, [&](IConst2) { return builder.getInt32(2); },
                [&](IConst3) { return builder.getInt32(3); }, [&](IConst4) { return builder.getInt32(4); },
                [&](IConst5) { return builder.getInt32(5); }, [&](LConst0) { return builder.getInt64(0); },
                [&](LConst1) { return builder.getInt64(1); });

            operandStack.push_back(value);
        },
        [&](OneOf<DDiv, FDiv, IDiv, LDiv>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            auto* quotient = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid div operation"); },
                [&](OneOf<DDiv, FDiv>) { return builder.CreateFDiv(lhs, rhs); },
                [&](OneOf<IDiv, LDiv>) { return builder.CreateSDiv(lhs, rhs); });

            operandStack.push_back(quotient);
        },
        [&](OneOf<DMul, FMul, IMul, LMul>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            auto* product = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid mul operation"); },
                [&](OneOf<DMul, FMul>) { return builder.CreateFMul(lhs, rhs); },
                [&](OneOf<IMul, LMul>) { return builder.CreateMul(lhs, rhs); });

            operandStack.push_back(product);
        },
        [&](OneOf<DNeg, FNeg, INeg, LNeg>)
        {
            llvm::Value* value = operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid neg operation"); },
                [&](OneOf<DNeg, FNeg>) { return builder.CreateFNeg(value); },
                [&](OneOf<INeg, LNeg>) { return builder.CreateNeg(value); });

            operandStack.push_back(result);
        },
        [&](OneOf<DRem, FRem, IRem, LRem>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            auto* remainder = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid rem operation"); },
                [&](OneOf<DRem, FRem>) { return builder.CreateFRem(lhs, rhs); },
                [&](OneOf<IRem, LRem>) { return builder.CreateSRem(lhs, rhs); });

            operandStack.push_back(remainder);
        },
        [&](OneOf<DSub, FSub, ISub, LSub>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();

            auto* difference = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid sub operation"); },
                [&](OneOf<DSub, FSub>) { return builder.CreateFSub(lhs, rhs); },
                [&](OneOf<ISub, LSub>) { return builder.CreateSub(lhs, rhs); });

            operandStack.push_back(difference);
        },
        [&](Dup)
        {
            llvm::Value* val = operandStack.pop_back();
            operandStack.push_back(val);
            operandStack.push_back(val);
        },
        [&](DupX1)
        {
            llvm::Value* value1 = operandStack.pop_back();
            llvm::Value* value2 = operandStack.pop_back();

            assert(!isCategoryTwo(value1->getType()) && !isCategoryTwo(value2->getType()));

            operandStack.push_back(value1);
            operandStack.push_back(value2);
            operandStack.push_back(value1);
        },
        [&](DupX2)
        {
            auto [value1, type1] = operandStack.pop_back_with_type();
            auto [value2, type2] = operandStack.pop_back_with_type();

            if (!isCategoryTwo(type2))
            {
                // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                llvm::Value* value3 = operandStack.pop_back();

                operandStack.push_back(value1);
                operandStack.push_back(value3);
            }
            else
            {
                // Form 2: where value1 is a value of a category 1 computational type and value2 is a value of a
                // category 2 computational type
                operandStack.push_back(value1);
            }

            operandStack.push_back(value2);
            operandStack.push_back(value1);
        },
        [&](Dup2)
        {
            auto [value, type] = operandStack.pop_back_with_type();

            if (!isCategoryTwo(type))
            {
                // Form 1: where both value1 and value2 are values of a category 1 computational type
                llvm::Value* value2 = operandStack.pop_back();

                operandStack.push_back(value2);
                operandStack.push_back(value);
                operandStack.push_back(value2);
                operandStack.push_back(value);
            }
            else
            {
                // Form 2: where value is a value of a category 2 computational type
                operandStack.push_back(value);
                operandStack.push_back(value);
            }
        },
        [&](Dup2X1)
        {
            auto [value1, type1] = operandStack.pop_back_with_type();
            auto [value2, type2] = operandStack.pop_back_with_type();

            if (!isCategoryTwo(type1))
            {
                // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                llvm::Value* value3 = operandStack.pop_back();

                operandStack.push_back(value2);
                operandStack.push_back(value1);
                operandStack.push_back(value3);
            }
            else
            {
                // Form 2: where value1 is a value of a category 2 computational type and value2 is a value of a
                // category 1 computational type
                operandStack.push_back(value1);
            }

            operandStack.push_back(value2);
            operandStack.push_back(value1);
        },
        [&](Dup2X2)
        {
            auto [value1, type1] = operandStack.pop_back_with_type();
            auto [value2, type2] = operandStack.pop_back_with_type();

            if (!isCategoryTwo(type1))
            {
                auto [value3, type3] = operandStack.pop_back_with_type();

                if (!isCategoryTwo(type3))
                {
                    llvm::Value* value4 = operandStack.pop_back();

                    // Form 1: where value1, value2, value3, and value4 are all values of a category 1 computational
                    // type
                    operandStack.push_back(value2);
                    operandStack.push_back(value1);
                    operandStack.push_back(value4);
                }
                else
                {
                    // Form 3: where value1 and value2 are both values of a category 1 computational type and value3 is
                    // a value of a category 2 computational type:
                    operandStack.push_back(value2);
                    operandStack.push_back(value1);
                }

                operandStack.push_back(value3);
            }
            else
            {
                if (!isCategoryTwo(type2))
                {
                    llvm::Value* value3 = operandStack.pop_back();

                    // Form 2: where value1 is a value of a category 2 computational type and value2 and value3 are both
                    // values of a category 1 computational type
                    operandStack.push_back(value1);
                    operandStack.push_back(value3);
                }
                else
                {
                    // Form 4: where value1 and value2 are both values of a category 2 computational type
                    operandStack.push_back(value1);
                }
            }

            operandStack.push_back(value2);
            operandStack.push_back(value1);
        },
        [&](F2D)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateFPExt(value, builder.getDoubleTy()));
        },
        [&](GetField getField)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{getField.index}.resolve(m_classFile);
            const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(m_classFile);
            FieldType descriptor = parseFieldType(nameAndTypeInfo->descriptorIndex.resolve(m_classFile)->text);
            llvm::Type* type = descriptorToType(descriptor, builder.getContext());

            llvm::Value* objectRef = operandStack.pop_back();

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;
            llvm::Value* fieldOffset = m_helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldOffset))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::Value* fieldPtr = builder.CreateGEP(builder.getInt8Ty(), objectRef, {fieldOffset});
            llvm::Value* field = builder.CreateLoad(type, fieldPtr);
            if (const auto* baseType = get_if<BaseType>(&descriptor); baseType && baseType->getValue() < BaseType::Int)
            {
                // Extend to the operands stack i32 type.
                field = builder.CreateIntCast(field, builder.getInt32Ty(),
                                              /*isSigned=*/!baseType->isUnsigned());
            }

            operandStack.push_back(field);
        },
        [&](GetStatic getStatic)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{getStatic.index}.resolve(m_classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::Value* fieldPtr = m_helper.getStaticFieldAddress(builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldPtr))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            FieldType descriptor = parseFieldType(fieldType);
            llvm::Type* type = descriptorToType(descriptor, builder.getContext());
            llvm::Value* field = builder.CreateLoad(type, fieldPtr);
            if (const auto* baseType = get_if<BaseType>(&descriptor); baseType && baseType->getValue() < BaseType::Int)
            {
                // Extend to the operands stack i32 type.
                field = builder.CreateIntCast(field, builder.getInt32Ty(),
                                              /*isSigned=*/!baseType->isUnsigned());
            }
            operandStack.push_back(field);
        },
        [&](OneOf<Goto, GotoW> gotoOp)
        {
            auto index = gotoOp.target + gotoOp.offset;
            basicBlockStackStates.insert({basicBlocks[index], operandStack.saveState()});
            builder.CreateBr(basicBlocks[index]);
        },
        [&](I2B)
        {
            llvm::Value* value = operandStack.pop_back();
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt8Ty());
            operandStack.push_back(builder.CreateSExt(truncated, builder.getInt32Ty()));
        },
        [&](I2C)
        {
            llvm::Value* value = operandStack.pop_back();
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
            operandStack.push_back(builder.CreateZExt(truncated, builder.getInt32Ty()));
        },
        [&](OneOf<I2D, L2D>)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateSIToFP(value, builder.getDoubleTy()));
        },
        [&](OneOf<I2F, L2F>)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateSIToFP(value, builder.getFloatTy()));
        },
        [&](I2L)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateSExt(value, builder.getInt64Ty()));
        },
        [&](I2S)
        {
            llvm::Value* value = operandStack.pop_back();
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
            operandStack.push_back(builder.CreateSExt(truncated, builder.getInt32Ty()));
        },
        [&](IAnd)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();
            operandStack.push_back(builder.CreateAnd(lhs, rhs));
        },
        [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                  IfGe, IfGt, IfLe, IfNonNull, IfNull>
                cmpOp)
        {
            llvm::BasicBlock* basicBlock = basicBlocks[cmpOp.target + cmpOp.offset];
            llvm::BasicBlock* next = basicBlocks[cmpOp.offset + sizeof(OpCodes) + sizeof(int16_t)];

            llvm::Value* rhs;
            llvm::Value* lhs;
            llvm::CmpInst::Predicate predicate;

            match(
                operation, [](...) { llvm_unreachable("Invalid comparison operation"); },
                [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                {
                    rhs = operandStack.pop_back();
                    lhs = operandStack.pop_back();
                },
                [&](OneOf<IfEq, IfNe, IfLt, IfGe, IfGt, IfLe>)
                {
                    rhs = builder.getInt32(0);
                    lhs = operandStack.pop_back();
                },
                [&](OneOf<IfNonNull, IfNull>)
                {
                    lhs = operandStack.pop_back();
                    rhs = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(lhs->getType()));
                });

            match(
                operation, [](...) { llvm_unreachable("Invalid comparison operation"); },
                [&](OneOf<IfACmpEq, IfICmpEq, IfEq, IfNull>) { predicate = llvm::CmpInst::ICMP_EQ; },
                [&](OneOf<IfACmpNe, IfICmpNe, IfNe, IfNonNull>) { predicate = llvm::CmpInst::ICMP_NE; },
                [&](OneOf<IfICmpLt, IfLt>) { predicate = llvm::CmpInst::ICMP_SLT; },
                [&](OneOf<IfICmpLe, IfLe>) { predicate = llvm::CmpInst::ICMP_SLE; },
                [&](OneOf<IfICmpGt, IfGt>) { predicate = llvm::CmpInst::ICMP_SGT; },
                [&](OneOf<IfICmpGe, IfGe>) { predicate = llvm::CmpInst::ICMP_SGE; });

            llvm::Value* cond = builder.CreateICmp(predicate, lhs, rhs);
            basicBlockStackStates.insert({basicBlock, operandStack.saveState()});
            basicBlockStackStates.insert({next, operandStack.saveState()});
            builder.CreateCondBr(cond, basicBlock, next);
        },
        [&](IInc iInc)
        {
            llvm::Value* local = builder.CreateLoad(builder.getInt32Ty(), locals[iInc.index]);
            builder.CreateStore(builder.CreateAdd(local, builder.getInt32(iInc.byte)), locals[iInc.index]);
        },
        // TODO: InvokeDynamic
        [&](OneOf<InvokeInterface, InvokeVirtual> invoke)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(m_classFile);

            MethodType descriptor = parseMethodType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
            for (auto& iter : llvm::reverse(args))
            {
                iter = operandStack.pop_back();
            }
            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::FunctionType* functionType = descriptorToType(descriptor, false, builder.getContext());
            prepareArgumentsForCall(builder, args, functionType);

            llvm::Value* call = m_helper.doIndirectCall(builder, className, methodName, methodType, args,
                                                        holds_alternative<InvokeInterface>(operation) ?
                                                            LazyClassLoaderHelper::Interface :
                                                            LazyClassLoaderHelper::Virtual);

            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                operandStack.push_back(extendToStackType(builder, descriptor.returnType, call));
            }
        },
        [&](OneOf<InvokeSpecial, InvokeStatic> invoke)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(m_classFile);

            bool isStatic = holds_alternative<InvokeStatic>(operation);

            MethodType descriptor = parseMethodType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            std::vector<llvm::Value*> args(descriptor.parameters.size() + (isStatic ? 0 : /*objectref*/ 1));
            for (auto& iter : llvm::reverse(args))
            {
                iter = operandStack.pop_back();
            }

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::FunctionType* functionType = descriptorToType(descriptor, isStatic, builder.getContext());
            prepareArgumentsForCall(builder, args, functionType);

            llvm::Value* call = m_helper.doNonVirtualCall(builder, isStatic, className, methodName, methodType, args);
            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                operandStack.push_back(extendToStackType(builder, descriptor.returnType, call));
            }
        },
        [&](IOr)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();
            operandStack.push_back(builder.CreateOr(lhs, rhs));
        },
        [&](OneOf<IShl, IShr, IUShr>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* maskedRhs = builder.CreateAnd(
                rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
            llvm::Value* lhs = operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid shift operation"); },
                [&](IShl) { return builder.CreateShl(lhs, maskedRhs); },
                [&](IShr) { return builder.CreateAShr(lhs, maskedRhs); },
                [&](IUShr) { return builder.CreateLShr(lhs, maskedRhs); });

            operandStack.push_back(result);
        },
        [&](IXor)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* lhs = operandStack.pop_back();
            operandStack.push_back(builder.CreateXor(lhs, rhs));
        },
        // TODO: JSR
        // TODO: JSRw
        [&](L2I)
        {
            llvm::Value* value = operandStack.pop_back();
            operandStack.push_back(builder.CreateTrunc(value, builder.getInt32Ty()));
        },
        // TODO: LAnd
        // TODO: LCmp
        [&](OneOf<LDC, LDCW, LDC2W> ldc)
        {
            PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                      InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                pool{ldc.index};

            match(
                pool.resolve(m_classFile),
                [&](const IntegerInfo* integerInfo) { operandStack.push_back(builder.getInt32(integerInfo->value)); },
                [&](const FloatInfo* floatInfo)
                { operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), floatInfo->value)); },
                [&](const LongInfo* longInfo) { operandStack.push_back(builder.getInt64(longInfo->value)); },
                [&](const DoubleInfo* doubleInfo)
                { operandStack.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), doubleInfo->value)); },
                [&](const StringInfo* stringInfo)
                {
                    llvm::StringRef text = stringInfo->stringValue.resolve(m_classFile)->text;

                    String* string = stringInterner.intern(text);

                    operandStack.push_back(
                        builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(string)),
                                               referenceType(builder.getContext())));
                },
                [&](const ClassInfo*) { operandStack.push_back(loadClassObjectFromPool(ldc.index)); },
                [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
        },
        [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
        {
            llvm::Value* key = operandStack.pop_back();

            llvm::BasicBlock* defaultBlock = basicBlocks[switchOp.offset + switchOp.defaultOffset];
            basicBlockStackStates.insert({defaultBlock, operandStack.saveState()});

            auto* switchInst = builder.CreateSwitch(key, defaultBlock, switchOp.matchOffsetsPairs.size());

            for (auto [match, target] : switchOp.matchOffsetsPairs)
            {
                llvm::BasicBlock* targetBlock = basicBlocks[switchOp.offset + target];
                basicBlockStackStates.insert({targetBlock, operandStack.saveState()});

                switchInst->addCase(builder.getInt32(match), targetBlock);
            }
        },
        // TODO: LOr
        [&](OneOf<LShl, LShr, LUShr>)
        {
            llvm::Value* rhs = operandStack.pop_back();
            llvm::Value* maskedRhs = builder.CreateAnd(
                rhs, builder.getInt32(0x3F));     // According to JVM only the lower 6 bits shall be considered
            llvm::Value* extendedRhs = builder.CreateSExt(
                maskedRhs, builder.getInt64Ty()); // LLVM only accepts binary ops with the same types for both operands
            llvm::Value* lhs = operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid shift operation"); },
                [&](LShl) { return builder.CreateShl(lhs, extendedRhs); },
                [&](LShr) { return builder.CreateAShr(lhs, extendedRhs); },
                [&](LUShr) { return builder.CreateLShr(lhs, extendedRhs); });

            operandStack.push_back(result);
        },
        // TODO: LXor
        [&](OneOf<MonitorEnter, MonitorExit>)
        {
            // Pop object as is required by the instruction.
            // TODO: If we ever care about multi threading, this would require lazily creating a mutex and
            //  (un)locking it.
            operandStack.pop_back();
        },
        [&](MultiANewArray multiANewArray)
        {
            llvm::StringRef descriptor =
                PoolIndex<ClassInfo>{multiANewArray.index}.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;

            assert(descriptor.size() - descriptor.drop_while([](char c) { return c == '['; }).size()
                   == multiANewArray.dimensions);

            llvm::StringRef className = descriptor;
            std::uint8_t dimensions = multiANewArray.dimensions;
            std::uint8_t iterations = dimensions - 1;

            std::vector<llvm::BasicBlock*> loopStarts{iterations};
            std::vector<llvm::BasicBlock*> loopEnds{iterations};

            std::vector<llvm::Value*> loopCounts{dimensions};
            std::vector<llvm::Value*> arrayClassObjects{dimensions};

            std::generate(loopStarts.begin(), loopStarts.end(),
                          [&] { return llvm::BasicBlock::Create(builder.getContext(), "start", m_function); });

            std::generate(loopEnds.rbegin(), loopEnds.rend(),
                          [&] { return llvm::BasicBlock::Create(builder.getContext(), "end", m_function); });

            std::generate(loopCounts.rbegin(), loopCounts.rend(),
                          // TODO: throw NegativeArraySizeException
                          [&] { return operandStack.pop_back(); });

            std::generate(arrayClassObjects.begin(), arrayClassObjects.end(),
                          [&]
                          {
                              llvm::Value* classObject = m_helper.getClassObject(builder, descriptor);
                              descriptor = descriptor.drop_front();

                              return classObject;
                          });

            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(arrayClassObjects[0]))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::BasicBlock* done = llvm::BasicBlock::Create(builder.getContext(), "done", m_function);

            llvm::Value* size = loopCounts[0];
            llvm::Value* array = generateAllocArray(className, arrayClassObjects[0], size);
            llvm::Value* outerArray = array;
            llvm::BasicBlock* nextEnd = done;

            // in C++23: std::ranges::zip_transform_view
            for (int i = 0; i < iterations; i++)
            {
                llvm::BasicBlock* start = loopStarts[i];
                llvm::BasicBlock* end = loopEnds[i];
                llvm::BasicBlock* last = builder.GetInsertBlock();

                llvm::Value* innerSize = loopCounts[i + 1];
                llvm::Value* classObject = arrayClassObjects[i + 1];

                llvm::Value* cmp = builder.CreateICmpSGT(size, builder.getInt32(0));
                builder.CreateCondBr(cmp, start, nextEnd);

                builder.SetInsertPoint(start);

                llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
                phi->addIncoming(builder.getInt32(0), last);

                llvm::Value* innerArray = generateAllocArray(className.drop_front(), classObject, innerSize);

                llvm::Value* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), outerArray,
                                                     {builder.getInt32(0), builder.getInt32(2), phi});
                builder.CreateStore(innerArray, gep);

                builder.SetInsertPoint(end);

                llvm::Value* counter = builder.CreateAdd(phi, builder.getInt32(1));
                phi->addIncoming(counter, end);

                cmp = builder.CreateICmpEQ(counter, size);
                builder.CreateCondBr(cmp, nextEnd, start);

                builder.SetInsertPoint(start);
                className = className.drop_front();
                outerArray = innerArray;
                size = innerSize;
                nextEnd = end;
            }

            builder.CreateBr(loopEnds.back());
            builder.SetInsertPoint(done);

            operandStack.push_back(array);
        },
        [&](New newOp)
        {
            llvm::Value* classObject = loadClassObjectFromPool(newOp.index);

            // Size is first 4 bytes in the class object and does not include the object header.
            llvm::Value* fieldAreaPtr = builder.CreateGEP(builder.getInt8Ty(), classObject,
                                                          {builder.getInt32(ClassObject::getFieldAreaSizeOffset())});
            llvm::Value* size = builder.CreateLoad(builder.getInt32Ty(), fieldAreaPtr);
            size = builder.CreateAdd(size, builder.getInt32(sizeof(ObjectHeader)));

            llvm::Module* module = m_function->getParent();
            llvm::Value* object = builder.CreateCall(allocationFunction(module), size);
            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            // Store object header (which in our case is just the class object) in the object.
            builder.CreateStore(classObject, object);
            operandStack.push_back(object);
        },
        [&](NewArray newArray)
        {
            auto [descriptor, type, size, elementOffset] = resolveNewArrayInfo(newArray.atype, builder);
            // TODO: throw NegativeArraySizeException
            llvm::Value* count = operandStack.pop_back();

            llvm::Value* classObject = m_helper.getClassObject(builder, "[" + descriptor);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may
            // occur.
            if (!llvm::isa<llvm::Constant>(classObject))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            // Size required is the size of the array prior to the elements (equal to the offset to the
            // elements) plus element count * element size.
            llvm::Value* bytesNeeded = builder.getInt32(elementOffset);
            bytesNeeded = builder.CreateAdd(bytesNeeded, builder.CreateMul(count, builder.getInt32(size)));

            // Type object.
            llvm::Value* object = builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);

            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            builder.CreateStore(classObject, object);
            // Array length.
            llvm::Value* gep =
                builder.CreateGEP(arrayStructType(type), object, {builder.getInt32(0), builder.getInt32(1)});
            builder.CreateStore(count, gep);

            operandStack.push_back(object);
        },
        [](Nop) {}, [&](Pop) { operandStack.pop_back(); },
        [&](Pop2)
        {
            llvm::Type* type = operandStack.pop_back_with_type().second;
            if (!isCategoryTwo(type))
            {
                // Form 1: pop two values of a category 1 computational type
                operandStack.pop_back();
            }
        },
        [&](PutField putField)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{putField.index}.resolve(m_classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
            llvm::Value* value = operandStack.pop_back();
            llvm::Value* objectRef = operandStack.pop_back();
            llvm::Value* fieldOffset = m_helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldOffset))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::Value* fieldPtr =
                builder.CreateGEP(llvm::Type::getInt8Ty(builder.getContext()), objectRef, {fieldOffset});

            if (value->getType() != llvmFieldType)
            {
                // Truncated from the operands stack i32 type.
                assert(value->getType()->isIntegerTy() && llvmFieldType->isIntegerTy()
                       && value->getType()->getIntegerBitWidth() > llvmFieldType->getIntegerBitWidth());
                value = builder.CreateTrunc(value, llvmFieldType);
            }

            builder.CreateStore(value, fieldPtr);
        },
        [&](PutStatic putStatic)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{putStatic.index}.resolve(m_classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
            llvm::Value* value = operandStack.pop_back();
            llvm::Value* fieldPtr = m_helper.getStaticFieldAddress(builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldPtr))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            if (value->getType() != llvmFieldType)
            {
                // Truncated from the operands stack i32 type.
                assert(value->getType()->isIntegerTy() && llvmFieldType->isIntegerTy()
                       && value->getType()->getIntegerBitWidth() > llvmFieldType->getIntegerBitWidth());
                value = builder.CreateTrunc(value, llvmFieldType);
            }

            builder.CreateStore(value, fieldPtr);
        },
        // TODO: Ret
        [&](Return) { builder.CreateRetVoid(); },
        [&](SIPush siPush) { operandStack.push_back(builder.getInt32(siPush.value)); },
        [&](Swap)
        {
            llvm::Value* value1 = operandStack.pop_back();
            llvm::Value* value2 = operandStack.pop_back();

            operandStack.push_back(value1);
            operandStack.push_back(value2);
        },
        [&](Wide wide)
        {
            llvm::Type* type;
            switch (wide.opCode)
            {
                default: llvm_unreachable("Invalid wide operation");
                case OpCodes::AStore:
                case OpCodes::DStore:
                case OpCodes::FStore:
                case OpCodes::IStore:
                case OpCodes::LStore:
                {
                    builder.CreateStore(operandStack.pop_back(), locals[wide.index]);
                    return;
                }
                case OpCodes::Ret: llvm_unreachable("NOT YET IMPLEMENTED");
                case OpCodes::IInc:
                {
                    llvm::Value* local = builder.CreateLoad(builder.getInt32Ty(), locals[wide.index]);
                    builder.CreateStore(builder.CreateAdd(local, builder.getInt32(*wide.value)), locals[wide.index]);
                    return;
                }
                case OpCodes::ALoad:
                {
                    type = referenceType(builder.getContext());
                    break;
                }
                case OpCodes::DLoad:
                {
                    type = builder.getDoubleTy();
                    break;
                }
                case OpCodes::FLoad:
                {
                    type = builder.getFloatTy();
                    break;
                }
                case OpCodes::ILoad:
                {
                    type = builder.getInt32Ty();
                    break;
                }
                case OpCodes::LLoad:
                {
                    type = builder.getInt64Ty();
                    break;
                }
            }

            operandStack.push_back(builder.CreateLoad(type, locals[wide.index]));
        });
}

void CodeGenerator::generateEHDispatch()
{
    llvm::PointerType* referenceTy = referenceType(builder.getContext());
    llvm::Value* value = builder.CreateLoad(referenceTy, activeException(m_function->getParent()));
    llvm::Value* cond = builder.CreateICmpEQ(value, llvm::ConstantPointerNull::get(referenceTy));

    auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
    builder.CreateCondBr(cond, continueBlock, generateHandlerChain(value, builder.GetInsertBlock()));

    builder.SetInsertPoint(continueBlock);
}

llvm::BasicBlock* CodeGenerator::generateHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred)
{
    llvm::IRBuilder<>::InsertPointGuard guard{builder};

    auto result = m_alreadyGeneratedHandlers.find(activeHandlers);
    if (result != m_alreadyGeneratedHandlers.end())
    {
        llvm::BasicBlock* block = result->second;
        // Adding new predecessors exception object to phi node.
        llvm::cast<llvm::PHINode>(&block->front())->addIncoming(exception, newPred);
        return block;
    }

    auto* ehHandler = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
    m_alreadyGeneratedHandlers.emplace(activeHandlers, ehHandler);
    builder.SetInsertPoint(ehHandler);

    llvm::PHINode* phi = builder.CreatePHI(exception->getType(), 0);
    phi->addIncoming(exception, newPred);

    for (auto [handlerPC, catchType] : activeHandlers)
    {
        llvm::BasicBlock* handlerBB = basicBlocks[handlerPC];

        llvm::PointerType* ty = referenceType(builder.getContext());

        if (!catchType)
        {
            // Catch all used to implement 'finally'.
            // Set exception object as only object on the stack and clear the active exception.
            builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(m_function->getParent()));
            operandStack.setHandlerStack(phi);
            builder.CreateBr(handlerBB);
            return ehHandler;
        }

        // Since an exception class must be loaded for any instance of the class to be created, we can be
        // certain that the exception is not of the type if the class has not yet been loaded. And most
        // importantly, don't need to eagerly load it.
        llvm::FunctionCallee forNameLoaded =
            m_function->getParent()->getOrInsertFunction("jllvm_for_name_loaded", ty, builder.getPtrTy());
        llvm::SmallString<64> buffer;
        llvm::Value* className = builder.CreateGlobalStringPtr(
            ("L" + catchType.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text + ";").toStringRef(buffer));
        llvm::Value* classObject = builder.CreateCall(forNameLoaded, className);
        llvm::Value* notLoaded = builder.CreateICmpEQ(classObject, llvm::ConstantPointerNull::get(ty));

        auto* nextHandler = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
        auto* instanceOfCheck = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
        builder.CreateCondBr(notLoaded, nextHandler, instanceOfCheck);

        builder.SetInsertPoint(instanceOfCheck);

        llvm::FunctionCallee callee = m_function->getParent()->getOrInsertFunction(
            "jllvm_instance_of", builder.getInt32Ty(), ty, classObject->getType());
        llvm::Value* call = builder.CreateCall(callee, {phi, classObject});
        call = builder.CreateTrunc(call, builder.getInt1Ty());

        auto* jumpToHandler = llvm::BasicBlock::Create(builder.getContext(), "", m_function);
        builder.CreateCondBr(call, jumpToHandler, nextHandler);

        builder.SetInsertPoint(jumpToHandler);
        // Set exception object as only object on the stack and clear the active exception.
        operandStack.setHandlerStack(phi);
        builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(m_function->getParent()));
        builder.CreateBr(handlerBB);

        builder.SetInsertPoint(nextHandler);
    }

    // Otherwise, propagate exception to parent frame:

    llvm::Type* retType = builder.getCurrentFunctionReturnType();
    if (retType->isVoidTy())
    {
        builder.CreateRetVoid();
    }
    else
    {
        builder.CreateRet(llvm::UndefValue::get(retType));
    }

    return ehHandler;
}

llvm::Value* CodeGenerator::loadClassObjectFromPool(PoolIndex<ClassInfo> index)
{
    llvm::StringRef className = index.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
    // TODO: If we ever bother verifying class files then the below could throw verification related exceptions
    //       (not initialization related since those happen later).
    if (className.front() == '[')
    {
        // Weirdly, it uses normal field mangling if it's an array type, but for other class types it's
        // just the name of the class. Hence, these two cases.
        return m_helper.getClassObject(builder, className);
    }

    return m_helper.getClassObject(builder, "L" + className + ";");
}

llvm::Value* CodeGenerator::generateAllocArray(llvm::StringRef descriptor, llvm::Value* classObject, llvm::Value* size)
{
    auto [elementType, elementSize, elementOffset] = match(
        parseFieldType(descriptor.drop_front()),
        [&](BaseType baseType) -> std::tuple<llvm::Type*, std::size_t, std::size_t>
        {
            auto [_, eType, eSize, eOffset] =
                resolveNewArrayInfo(static_cast<ArrayOp::ArrayType>(baseType.getValue()), builder);
            return {eType, eSize, eOffset};
        },
        [&](auto) -> std::tuple<llvm::Type*, std::size_t, std::size_t> {
            return {referenceType(builder.getContext()), sizeof(Object*), Array<>::arrayElementsOffset()};
        });

    // Size required is the size of the array prior to the elements (equal to the offset to the
    // elements) plus element count * element size.
    llvm::Value* bytesNeeded =
        builder.CreateAdd(builder.getInt32(elementOffset), builder.CreateMul(size, builder.getInt32(elementSize)));

    // TODO: Allocation can throw OutOfMemoryException, create EH-dispatch
    llvm::Value* array = builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);

    builder.CreateStore(classObject, array);

    llvm::Value* gep =
        builder.CreateGEP(arrayStructType(elementType), array, {builder.getInt32(0), builder.getInt32(1)});
    builder.CreateStore(size, gep);

    return array;
}
