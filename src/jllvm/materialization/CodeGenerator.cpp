#include "CodeGenerator.hpp"

#include <llvm/Support/ModRef.h>

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
    function->addRetAttrs(llvm::AttrBuilder(module->getContext())
                              .addAlignmentAttr(alignof(ObjectHeader))
                              .addAttribute(llvm::Attribute::NonNull)
                              .addAttribute(llvm::Attribute::NoUndef));
    return function;
}

llvm::FunctionCallee instanceOfFunction(llvm::Module* module)
{
    auto* function = module->getFunction("jllvm_instance_of");
    if (function)
    {
        return function;
    }

    llvm::Type* ty = referenceType(module->getContext());
    function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::IntegerType::get(module->getContext(), 32), {ty, ty}, false),
        llvm::GlobalValue::ExternalLinkage, "jllvm_instance_of", module);
    function->addFnAttrs(llvm::AttrBuilder(module->getContext())
                             .addAttribute("gc-leaf-function")
                             .addMemoryAttr(llvm::MemoryEffects::readOnly())
                             .addAttribute(llvm::Attribute::WillReturn)
                             .addAttribute(llvm::Attribute::NoUnwind));
    function->addParamAttr(0, llvm::Attribute::NoCapture);
    function->addParamAttr(1, llvm::Attribute::NoCapture);
    function->addRetAttrs(llvm::AttrBuilder(module->getContext()).addAttribute(llvm::Attribute::NoUndef));
    return function;
}

llvm::FunctionCallee forNameLoadedFunction(llvm::Module* module)
{
    auto* function = module->getFunction("jllvm_for_name_loaded");
    if (function)
    {
        return function;
    }

    llvm::Type* ty = referenceType(module->getContext());
    function =
        llvm::Function::Create(llvm::FunctionType::get(ty, {llvm::PointerType::get(module->getContext(), 0)}, false),
                               llvm::GlobalValue::ExternalLinkage, "jllvm_for_name_loaded", module);
    function->addFnAttrs(llvm::AttrBuilder(module->getContext())
                             .addAttribute("gc-leaf-function")
                             .addAttribute(llvm::Attribute::NoUnwind)
                             .addMemoryAttr(llvm::MemoryEffects::inaccessibleOrArgMemOnly()));
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
    llvm::DIFile* file = m_debugBuilder.createFile("temp.java", ".");
    llvm::DICompileUnit* cu = m_debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

    llvm::DISubprogram* subprogram =
        m_debugBuilder.createFunction(file, m_function->getName(), "", file, 1,
                                      m_debugBuilder.createSubroutineType(m_debugBuilder.getOrCreateTypeArray({})), 1,
                                      llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    m_function->setSubprogram(subprogram);

    // We need pointer size bytes, since that is the largest type we may store in a local.
    std::generate(m_locals.begin(), m_locals.end(), [&] { return m_builder.CreateAlloca(m_builder.getPtrTy()); });

    // Arguments are put into the locals. According to the specification, i64s and doubles are split into two
    // locals. We don't actually do that, we just put them into the very first local, but we still have to skip over
    // the following local as if we didn't.
    auto nextLocal = m_locals.begin();
    for (auto& arg : m_function->args())
    {
        m_builder.CreateStore(&arg, *nextLocal++);
        if (arg.getType()->isIntegerTy(64) || arg.getType()->isDoubleTy())
        {
            nextLocal++;
        }
    }

    calculateBasicBlocks(code);
    generateCodeBody(code);

    m_debugBuilder.finalizeSubprogram(subprogram);
    m_debugBuilder.finalize();
}

void CodeGenerator::calculateBasicBlocks(const Code& code)
{
    for (ByteCodeOp operation : byteCodeRange(code.getCode()))
    {
        auto addBasicBlock = [&](std::uint16_t target)
        {
            auto [result, inserted] = m_basicBlocks.insert({target, nullptr});

            if (inserted)
            {
                result->second = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
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
        auto [result, inserted] = m_basicBlocks.insert({iter.handlerPc, nullptr});
        if (!inserted)
        {
            continue;
        }
        // Handlers have the special semantic of only having the caught exception at the very top. It is therefore
        // required that we register that fact in 'basicBlockStackStates' explicitly.
        result->second = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
        m_basicBlockStackStates.insert({result->second, m_operandStack.getHandlerState()});
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
                m_activeHandlers.erase(iter);
            }
            // No longer needed.
            endHandlers.erase(result);
        }

        if (auto result = startHandlers.find(offset); result != startHandlers.end())
        {
            for (const Code::ExceptionTable& iter : result->second)
            {
                m_activeHandlers.emplace_back(iter.handlerPc, iter.catchType);
                endHandlers[iter.endPc].push_back(std::prev(m_activeHandlers.end()));
            }
            // No longer needed.
            startHandlers.erase(result);
        }

        if (auto result = m_basicBlocks.find(offset); result != m_basicBlocks.end())
        {
            // Without any branches, there will not be a terminator at the end of the basic block. Thus, we need to
            // set this manually to the new insert point. This essentially implements implicit fallthrough from JVM
            // bytecode.
            if (m_builder.GetInsertBlock()->getTerminator() == nullptr)
            {
                m_basicBlockStackStates.insert({result->second, m_operandStack.saveState()});
                m_builder.CreateBr(result->second);
            }
            m_builder.SetInsertPoint(result->second);
            if (auto resultStackPointer = m_basicBlockStackStates.find(result->second);
                resultStackPointer != m_basicBlockStackStates.end())
            {
                m_operandStack.restoreState(resultStackPointer->second);
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
                [&](AALoad) -> llvm::Type* { return referenceType(m_builder.getContext()); },
                [&](BALoad) { return m_builder.getInt8Ty(); },
                [&](OneOf<CALoad, SALoad>) { return m_builder.getInt16Ty(); },
                [&](DALoad) { return m_builder.getDoubleTy(); }, [&](FALoad) { return m_builder.getFloatTy(); },
                [&](IALoad) { return m_builder.getInt32Ty(); }, [&](LALoad) { return m_builder.getInt64Ty(); });

            llvm::Value* index = m_operandStack.pop_back();
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = m_operandStack.pop_back();

            // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
            llvm::Value* gep = m_builder.CreateGEP(arrayStructType(type), array,
                                                   {m_builder.getInt32(0), m_builder.getInt32(2), index});
            llvm::Value* value = m_builder.CreateLoad(type, gep);

            match(
                operation, [](...) {},
                [&](OneOf<BALoad, SALoad>) { value = m_builder.CreateSExt(value, m_builder.getInt32Ty()); },
                [&](CALoad) { value = m_builder.CreateZExt(value, m_builder.getInt32Ty()); });

            m_operandStack.push_back(value);
        },
        [&](OneOf<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid array load operation"); },
                [&](AAStore) { return referenceType(m_builder.getContext()); },
                [&](BAStore) { return m_builder.getInt8Ty(); },
                [&](OneOf<CAStore, SAStore>) { return m_builder.getInt16Ty(); },
                [&](DAStore) { return m_builder.getDoubleTy(); }, [&](FAStore) { return m_builder.getFloatTy(); },
                [&](IAStore) { return m_builder.getInt32Ty(); }, [&](LAStore) { return m_builder.getInt64Ty(); });

            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* index = m_operandStack.pop_back();
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = m_operandStack.pop_back();

            // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
            llvm::Value* gep = m_builder.CreateGEP(arrayStructType(type), array,
                                                   {m_builder.getInt32(0), m_builder.getInt32(2), index});
            match(
                operation, [](...) {},
                [&, arrayType = type](OneOf<BAStore, CAStore, SAStore>)
                { value = m_builder.CreateTrunc(value, arrayType); });

            m_builder.CreateStore(value, gep);
        },
        [&](AConstNull)
        { m_operandStack.push_back(llvm::ConstantPointerNull::get(referenceType(m_builder.getContext()))); },
        [&](OneOf<ALoad, DLoad, FLoad, ILoad, LLoad> load)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid load operation"); },
                [&](ALoad) { return referenceType(m_builder.getContext()); },
                [&](DLoad) { return m_builder.getDoubleTy(); }, [&](FLoad) { return m_builder.getFloatTy(); },
                [&](ILoad) { return m_builder.getInt32Ty(); }, [&](LLoad) { return m_builder.getInt64Ty(); });

            m_operandStack.push_back(m_builder.CreateLoad(type, m_locals[load.index]));
        },
        [&](OneOf<ALoad0, DLoad0, FLoad0, ILoad0, LLoad0, ALoad1, DLoad1, FLoad1, ILoad1, LLoad1, ALoad2, DLoad2,
                  FLoad2, ILoad2, LLoad2, ALoad3, DLoad3, FLoad3, ILoad3, LLoad3>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid load operation"); },
                [&](OneOf<ALoad0, ALoad1, ALoad2, ALoad3>) { return referenceType(m_builder.getContext()); },
                [&](OneOf<DLoad0, DLoad1, DLoad2, DLoad3>) { return m_builder.getDoubleTy(); },
                [&](OneOf<FLoad0, FLoad1, FLoad2, FLoad3>) { return m_builder.getFloatTy(); },
                [&](OneOf<ILoad0, ILoad1, ILoad2, ILoad3>) { return m_builder.getInt32Ty(); },
                [&](OneOf<LLoad0, LLoad1, LLoad2, LLoad3>) { return m_builder.getInt64Ty(); });

            auto index = match(
                operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid load operation"); },
                [&](OneOf<ALoad0, DLoad0, FLoad0, ILoad0, LLoad0>) { return 0; },
                [&](OneOf<ALoad1, DLoad1, FLoad1, ILoad1, LLoad1>) { return 1; },
                [&](OneOf<ALoad2, DLoad2, FLoad2, ILoad2, LLoad2>) { return 2; },
                [&](OneOf<ALoad3, DLoad3, FLoad3, ILoad3, LLoad3>) { return 3; });

            m_operandStack.push_back(m_builder.CreateLoad(type, m_locals[index]));
        },
        [&](ANewArray aNewArray)
        {
            auto index = PoolIndex<ClassInfo>{aNewArray.index};
            // TODO: throw NegativeArraySizeException
            llvm::Value* count = m_operandStack.pop_back();

            llvm::Value* classObject = m_helper.getClassObject(
                m_builder, "[L" + index.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text + ";");

            // Size required is the size of the array prior to the elements (equal to the offset to the
            // elements) plus element count * element size.
            llvm::Value* bytesNeeded = m_builder.getInt32(Array<>::arrayElementsOffset());
            bytesNeeded =
                m_builder.CreateAdd(bytesNeeded, m_builder.CreateMul(count, m_builder.getInt32(sizeof(Object*))));

            llvm::Value* object = m_builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);
            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            // Type object.
            m_builder.CreateStore(classObject, object);
            // Array length.
            auto* gep = m_builder.CreateGEP(arrayStructType(referenceType(m_builder.getContext())), object,
                                            {m_builder.getInt32(0), m_builder.getInt32(1)});
            m_builder.CreateStore(count, gep);

            m_operandStack.push_back(object);
        },
        [&](OneOf<AReturn, DReturn, FReturn, IReturn, LReturn>)
        {
            llvm::Value* value = m_operandStack.pop_back();

            match(
                operation, [](...) {},
                [&](IReturn)
                {
                    if (m_functionMethodType.returnType == FieldType(BaseType::Boolean))
                    {
                        value = m_builder.CreateAnd(value, m_builder.getInt32(1));
                    }
                    if (m_function->getReturnType() != value->getType())
                    {
                        value = m_builder.CreateTrunc(value, m_function->getReturnType());
                    }
                });

            m_builder.CreateRet(value);
        },
        [&](ArrayLength)
        {
            llvm::Value* array = m_operandStack.pop_back();

            // The element type of the array type here is actually irrelevant.
            llvm::Value* gep = m_builder.CreateGEP(arrayStructType(referenceType(m_builder.getContext())), array,
                                                   {m_builder.getInt32(0), m_builder.getInt32(1)});
            m_operandStack.push_back(m_builder.CreateLoad(m_builder.getInt32Ty(), gep));
        },
        [&](OneOf<AStore, DStore, FStore, IStore, LStore> store)
        { m_builder.CreateStore(m_operandStack.pop_back(), m_locals[store.index]); },
        [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0, AStore1, DStore1, FStore1, IStore1, LStore1, AStore2,
                  DStore2, FStore2, IStore2, LStore2, AStore3, DStore3, FStore3, IStore3, LStore3>)
        {
            auto index = match(
                operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0>) { return 0; },
                [&](OneOf<AStore1, DStore1, FStore1, IStore1, LStore1>) { return 1; },
                [&](OneOf<AStore2, DStore2, FStore2, IStore2, LStore2>) { return 2; },
                [&](OneOf<AStore3, DStore3, FStore3, IStore3, LStore3>) { return 3; });

            m_builder.CreateStore(m_operandStack.pop_back(), m_locals[index]);
        },
        [&](AThrow)
        {
            llvm::Value* exception = m_operandStack.pop_back();

            m_builder.CreateStore(exception, activeException(m_function->getParent()));

            m_builder.CreateBr(generateHandlerChain(exception, m_builder.GetInsertBlock()));
        },
        [&](BIPush biPush)
        {
            llvm::Value* res = m_builder.getInt32(biPush.value);
            m_operandStack.push_back(res);
        },
        [&](OneOf<CheckCast, InstanceOf> op)
        {
            llvm::PointerType* ty = referenceType(m_builder.getContext());
            llvm::Value* object = m_operandStack.pop_back();
            llvm::Value* null = llvm::ConstantPointerNull::get(ty);

            llvm::Value* isNull = m_builder.CreateICmpEQ(object, null);
            auto* continueBlock = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
            auto* instanceOfBlock = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
            llvm::BasicBlock* block = m_builder.GetInsertBlock();
            m_builder.CreateCondBr(isNull, continueBlock, instanceOfBlock);

            m_builder.SetInsertPoint(instanceOfBlock);

            llvm::Value* classObject = loadClassObjectFromPool(op.index);

            llvm::Instruction* call =
                m_builder.CreateCall(instanceOfFunction(m_function->getParent()), {object, classObject});

            match(
                operation, [](...) { llvm_unreachable("Invalid operation"); },
                [&](InstanceOf)
                {
                    m_builder.CreateBr(continueBlock);

                    m_builder.SetInsertPoint(continueBlock);
                    llvm::PHINode* phi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
                    // null references always return 0.
                    phi->addIncoming(m_builder.getInt32(0), block);
                    phi->addIncoming(call, call->getParent());

                    m_operandStack.push_back(phi);
                },
                [&](CheckCast)
                {
                    m_operandStack.push_back(object);
                    auto* throwBlock = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
                    m_builder.CreateCondBr(m_builder.CreateTrunc(call, m_builder.getInt1Ty()), continueBlock,
                                           throwBlock);

                    m_builder.SetInsertPoint(throwBlock);

                    llvm::Value* exception = m_builder.CreateCall(
                        m_function->getParent()->getOrInsertFunction("jllvm_build_class_cast_exception",
                                                                     llvm::FunctionType::get(ty, {ty, ty}, false)),
                        {object, classObject});

                    m_builder.CreateStore(exception, activeException(m_function->getParent()));

                    m_builder.CreateBr(generateHandlerChain(exception, m_builder.GetInsertBlock()));

                    m_builder.SetInsertPoint(continueBlock);
                });
        },
        [&](D2F)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateFPTrunc(value, m_builder.getFloatTy()));
        },
        [&](OneOf<D2I, D2L, F2I, F2L>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid conversion operation"); },
                [&](OneOf<D2I, F2I>) { return m_builder.getInt32Ty(); },
                [&](OneOf<D2L, F2L>) { return m_builder.getInt64Ty(); });

            llvm::Value* value = m_operandStack.pop_back();

            m_operandStack.push_back(m_builder.CreateIntrinsic(type, llvm::Intrinsic::fptosi_sat, {value}));
        },
        [&](OneOf<DAdd, FAdd, IAdd, LAdd>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* sum = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid add operation"); },
                [&](OneOf<DAdd, FAdd>) { return m_builder.CreateFAdd(lhs, rhs); },
                [&](OneOf<IAdd, LAdd>) { return m_builder.CreateAdd(lhs, rhs); });

            m_operandStack.push_back(sum);
        },
        [&](OneOf<DCmpG, DCmpL, FCmpG, FCmpL>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            // using unordered compare to allow for NaNs
            // if lhs == rhs result is 0, otherwise the resulting boolean is converted for the default case
            llvm::Value* notEqual = m_builder.CreateFCmpUNE(lhs, rhs);
            llvm::Value* otherCmp;
            llvm::Value* otherCase;

            if (holds_alternative<FCmpG>(operation) || holds_alternative<DCmpG>(operation))
            {
                // is 0 if lhs == rhs, otherwise 1 for lhs > rhs or either operand being NaN
                notEqual = m_builder.CreateZExt(notEqual, m_builder.getInt32Ty());
                // using ordered less than to check lhs < rhs
                otherCmp = m_builder.CreateFCmpOLT(lhs, rhs);
                // return -1 if lhs < rhs
                otherCase = m_builder.getInt32(-1);
            }
            else
            {
                // is 0 if lhs == rhs, otherwise -1 for lhs < rhs or either operand being NaN
                notEqual = m_builder.CreateSExt(notEqual, m_builder.getInt32Ty());
                // using ordered greater than to check lhs > rhs
                otherCmp = m_builder.CreateFCmpOGT(lhs, rhs);
                // return -1 if lhs > rhs
                otherCase = m_builder.getInt32(1);
            }

            // select the non-default or the 0-or-default value based on the result of otherCmp
            m_operandStack.push_back(m_builder.CreateSelect(otherCmp, otherCase, notEqual));
        },
        [&](OneOf<DConst0, DConst1, FConst0, FConst1, FConst2, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4,
                  IConst5, LConst0, LConst1>)
        {
            auto* value = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid const operation"); },
                [&](DConst0) { return llvm::ConstantFP::get(m_builder.getDoubleTy(), 0.0); },
                [&](DConst1) { return llvm::ConstantFP::get(m_builder.getDoubleTy(), 1.0); },
                [&](FConst0) { return llvm::ConstantFP::get(m_builder.getFloatTy(), 0.0); },
                [&](FConst1) { return llvm::ConstantFP::get(m_builder.getFloatTy(), 1.0); },
                [&](FConst2) { return llvm::ConstantFP::get(m_builder.getFloatTy(), 2.0); },
                [&](IConstM1) { return m_builder.getInt32(-1); }, [&](IConst0) { return m_builder.getInt32(0); },
                [&](IConst1) { return m_builder.getInt32(1); }, [&](IConst2) { return m_builder.getInt32(2); },
                [&](IConst3) { return m_builder.getInt32(3); }, [&](IConst4) { return m_builder.getInt32(4); },
                [&](IConst5) { return m_builder.getInt32(5); }, [&](LConst0) { return m_builder.getInt64(0); },
                [&](LConst1) { return m_builder.getInt64(1); });

            m_operandStack.push_back(value);
        },
        [&](OneOf<DDiv, FDiv, IDiv, LDiv>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* quotient = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid div operation"); },
                [&](OneOf<DDiv, FDiv>) { return m_builder.CreateFDiv(lhs, rhs); },
                [&](OneOf<IDiv, LDiv>) { return m_builder.CreateSDiv(lhs, rhs); });

            m_operandStack.push_back(quotient);
        },
        [&](OneOf<DMul, FMul, IMul, LMul>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* product = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid mul operation"); },
                [&](OneOf<DMul, FMul>) { return m_builder.CreateFMul(lhs, rhs); },
                [&](OneOf<IMul, LMul>) { return m_builder.CreateMul(lhs, rhs); });

            m_operandStack.push_back(product);
        },
        [&](OneOf<DNeg, FNeg, INeg, LNeg>)
        {
            llvm::Value* value = m_operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid neg operation"); },
                [&](OneOf<DNeg, FNeg>) { return m_builder.CreateFNeg(value); },
                [&](OneOf<INeg, LNeg>) { return m_builder.CreateNeg(value); });

            m_operandStack.push_back(result);
        },
        [&](OneOf<DRem, FRem, IRem, LRem>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* remainder = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid rem operation"); },
                [&](OneOf<DRem, FRem>) { return m_builder.CreateFRem(lhs, rhs); },
                [&](OneOf<IRem, LRem>) { return m_builder.CreateSRem(lhs, rhs); });

            m_operandStack.push_back(remainder);
        },
        [&](OneOf<DSub, FSub, ISub, LSub>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* difference = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid sub operation"); },
                [&](OneOf<DSub, FSub>) { return m_builder.CreateFSub(lhs, rhs); },
                [&](OneOf<ISub, LSub>) { return m_builder.CreateSub(lhs, rhs); });

            m_operandStack.push_back(difference);
        },
        [&](Dup)
        {
            llvm::Value* val = m_operandStack.pop_back();
            m_operandStack.push_back(val);
            m_operandStack.push_back(val);
        },
        [&](DupX1)
        {
            llvm::Value* value1 = m_operandStack.pop_back();
            llvm::Value* value2 = m_operandStack.pop_back();

            assert(!isCategoryTwo(value1->getType()) && !isCategoryTwo(value2->getType()));

            m_operandStack.push_back(value1);
            m_operandStack.push_back(value2);
            m_operandStack.push_back(value1);
        },
        [&](DupX2)
        {
            auto [value1, type1] = m_operandStack.pop_back_with_type();
            auto [value2, type2] = m_operandStack.pop_back_with_type();

            if (!isCategoryTwo(type2))
            {
                // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                llvm::Value* value3 = m_operandStack.pop_back();

                m_operandStack.push_back(value1);
                m_operandStack.push_back(value3);
            }
            else
            {
                // Form 2: where value1 is a value of a category 1 computational type and value2 is a value of a
                // category 2 computational type
                m_operandStack.push_back(value1);
            }

            m_operandStack.push_back(value2);
            m_operandStack.push_back(value1);
        },
        [&](Dup2)
        {
            auto [value, type] = m_operandStack.pop_back_with_type();

            if (!isCategoryTwo(type))
            {
                // Form 1: where both value1 and value2 are values of a category 1 computational type
                llvm::Value* value2 = m_operandStack.pop_back();

                m_operandStack.push_back(value2);
                m_operandStack.push_back(value);
                m_operandStack.push_back(value2);
                m_operandStack.push_back(value);
            }
            else
            {
                // Form 2: where value is a value of a category 2 computational type
                m_operandStack.push_back(value);
                m_operandStack.push_back(value);
            }
        },
        [&](Dup2X1)
        {
            auto [value1, type1] = m_operandStack.pop_back_with_type();
            auto [value2, type2] = m_operandStack.pop_back_with_type();

            if (!isCategoryTwo(type1))
            {
                // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                llvm::Value* value3 = m_operandStack.pop_back();

                m_operandStack.push_back(value2);
                m_operandStack.push_back(value1);
                m_operandStack.push_back(value3);
            }
            else
            {
                // Form 2: where value1 is a value of a category 2 computational type and value2 is a value of a
                // category 1 computational type
                m_operandStack.push_back(value1);
            }

            m_operandStack.push_back(value2);
            m_operandStack.push_back(value1);
        },
        [&](Dup2X2)
        {
            auto [value1, type1] = m_operandStack.pop_back_with_type();
            auto [value2, type2] = m_operandStack.pop_back_with_type();

            if (!isCategoryTwo(type1))
            {
                auto [value3, type3] = m_operandStack.pop_back_with_type();

                if (!isCategoryTwo(type3))
                {
                    llvm::Value* value4 = m_operandStack.pop_back();

                    // Form 1: where value1, value2, value3, and value4 are all values of a category 1 computational
                    // type
                    m_operandStack.push_back(value2);
                    m_operandStack.push_back(value1);
                    m_operandStack.push_back(value4);
                }
                else
                {
                    // Form 3: where value1 and value2 are both values of a category 1 computational type and value3 is
                    // a value of a category 2 computational type:
                    m_operandStack.push_back(value2);
                    m_operandStack.push_back(value1);
                }

                m_operandStack.push_back(value3);
            }
            else
            {
                if (!isCategoryTwo(type2))
                {
                    llvm::Value* value3 = m_operandStack.pop_back();

                    // Form 2: where value1 is a value of a category 2 computational type and value2 and value3 are both
                    // values of a category 1 computational type
                    m_operandStack.push_back(value1);
                    m_operandStack.push_back(value3);
                }
                else
                {
                    // Form 4: where value1 and value2 are both values of a category 2 computational type
                    m_operandStack.push_back(value1);
                }
            }

            m_operandStack.push_back(value2);
            m_operandStack.push_back(value1);
        },
        [&](F2D)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateFPExt(value, m_builder.getDoubleTy()));
        },
        [&](GetField getField)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{getField.index}.resolve(m_classFile);
            const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(m_classFile);
            FieldType descriptor = parseFieldType(nameAndTypeInfo->descriptorIndex.resolve(m_classFile)->text);
            llvm::Type* type = descriptorToType(descriptor, m_builder.getContext());

            llvm::Value* objectRef = m_operandStack.pop_back();

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;
            llvm::Value* fieldOffset = m_helper.getInstanceFieldOffset(m_builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldOffset))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::Value* fieldPtr = m_builder.CreateGEP(m_builder.getInt8Ty(), objectRef, {fieldOffset});
            llvm::Value* field = m_builder.CreateLoad(type, fieldPtr);

            m_operandStack.push_back(extendToStackType(m_builder, descriptor, field));
        },
        [&](GetStatic getStatic)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{getStatic.index}.resolve(m_classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::Value* fieldPtr = m_helper.getStaticFieldAddress(m_builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldPtr))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            FieldType descriptor = parseFieldType(fieldType);
            llvm::Type* type = descriptorToType(descriptor, m_builder.getContext());
            llvm::Value* field = m_builder.CreateLoad(type, fieldPtr);

            m_operandStack.push_back(extendToStackType(m_builder, descriptor, field));
        },
        [&](OneOf<Goto, GotoW> gotoOp)
        {
            auto index = gotoOp.target + gotoOp.offset;
            m_basicBlockStackStates.insert({m_basicBlocks[index], m_operandStack.saveState()});
            m_builder.CreateBr(m_basicBlocks[index]);
        },
        [&](I2B)
        {
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* truncated = m_builder.CreateTrunc(value, m_builder.getInt8Ty());
            m_operandStack.push_back(m_builder.CreateSExt(truncated, m_builder.getInt32Ty()));
        },
        [&](I2C)
        {
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* truncated = m_builder.CreateTrunc(value, m_builder.getInt16Ty());
            m_operandStack.push_back(m_builder.CreateZExt(truncated, m_builder.getInt32Ty()));
        },
        [&](OneOf<I2D, L2D>)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateSIToFP(value, m_builder.getDoubleTy()));
        },
        [&](OneOf<I2F, L2F>)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateSIToFP(value, m_builder.getFloatTy()));
        },
        [&](I2L)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateSExt(value, m_builder.getInt64Ty()));
        },
        [&](I2S)
        {
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* truncated = m_builder.CreateTrunc(value, m_builder.getInt16Ty());
            m_operandStack.push_back(m_builder.CreateSExt(truncated, m_builder.getInt32Ty()));
        },
        [&](OneOf<IAnd, LAnd>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateAnd(lhs, rhs));
        },
        [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                  IfGe, IfGt, IfLe, IfNonNull, IfNull>
                cmpOp)
        {
            llvm::BasicBlock* basicBlock = m_basicBlocks[cmpOp.target + cmpOp.offset];
            llvm::BasicBlock* next = m_basicBlocks[cmpOp.offset + sizeof(OpCodes) + sizeof(int16_t)];

            llvm::Value* rhs;
            llvm::Value* lhs;
            llvm::CmpInst::Predicate predicate;

            match(
                operation, [](...) { llvm_unreachable("Invalid comparison operation"); },
                [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                {
                    rhs = m_operandStack.pop_back();
                    lhs = m_operandStack.pop_back();
                },
                [&](OneOf<IfEq, IfNe, IfLt, IfGe, IfGt, IfLe>)
                {
                    rhs = m_builder.getInt32(0);
                    lhs = m_operandStack.pop_back();
                },
                [&](OneOf<IfNonNull, IfNull>)
                {
                    lhs = m_operandStack.pop_back();
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

            llvm::Value* cond = m_builder.CreateICmp(predicate, lhs, rhs);
            m_basicBlockStackStates.insert({basicBlock, m_operandStack.saveState()});
            m_basicBlockStackStates.insert({next, m_operandStack.saveState()});
            m_builder.CreateCondBr(cond, basicBlock, next);
        },
        [&](IInc iInc)
        {
            llvm::Value* local = m_builder.CreateLoad(m_builder.getInt32Ty(), m_locals[iInc.index]);
            m_builder.CreateStore(m_builder.CreateAdd(local, m_builder.getInt32(iInc.byte)), m_locals[iInc.index]);
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
                iter = m_operandStack.pop_back();
            }
            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::FunctionType* functionType = descriptorToType(descriptor, false, m_builder.getContext());
            prepareArgumentsForCall(m_builder, args, functionType);

            llvm::Value* call = m_helper.doIndirectCall(m_builder, className, methodName, methodType, args,
                                                        holds_alternative<InvokeInterface>(operation) ?
                                                            LazyClassLoaderHelper::Interface :
                                                            LazyClassLoaderHelper::Virtual);

            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                m_operandStack.push_back(extendToStackType(m_builder, descriptor.returnType, call));
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
                iter = m_operandStack.pop_back();
            }

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;

            llvm::FunctionType* functionType = descriptorToType(descriptor, isStatic, m_builder.getContext());
            prepareArgumentsForCall(m_builder, args, functionType);

            llvm::Value* call = m_helper.doNonVirtualCall(m_builder, isStatic, className, methodName, methodType, args);
            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                m_operandStack.push_back(extendToStackType(m_builder, descriptor.returnType, call));
            }
        },
        [&](OneOf<IOr, LOr>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateOr(lhs, rhs));
        },
        [&](OneOf<IShl, IShr, IUShr>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* maskedRhs = m_builder.CreateAnd(
                rhs, m_builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid shift operation"); },
                [&](IShl) { return m_builder.CreateShl(lhs, maskedRhs); },
                [&](IShr) { return m_builder.CreateAShr(lhs, maskedRhs); },
                [&](IUShr) { return m_builder.CreateLShr(lhs, maskedRhs); });

            m_operandStack.push_back(result);
        },
        [&](OneOf<IXor, LXor>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateXor(lhs, rhs));
        },
        // TODO: JSR
        // TODO: JSRw
        [&](L2I)
        {
            llvm::Value* value = m_operandStack.pop_back();
            m_operandStack.push_back(m_builder.CreateTrunc(value, m_builder.getInt32Ty()));
        },
        [&](LCmp)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* lhs = m_operandStack.pop_back();
            llvm::Value* notEqual = m_builder.CreateICmpNE(lhs, rhs); // false if equal => 0
            notEqual = m_builder.CreateZExt(notEqual, m_builder.getInt32Ty());
            llvm::Value* otherCmp = m_builder.CreateICmpSLT(lhs, rhs);
            llvm::Value* otherCase = m_builder.getInt32(-1);
            m_operandStack.push_back(m_builder.CreateSelect(otherCmp, otherCase, notEqual));
        },
        [&](OneOf<LDC, LDCW, LDC2W> ldc)
        {
            PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                      InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                pool{ldc.index};

            match(
                pool.resolve(m_classFile),
                [&](const IntegerInfo* integerInfo)
                { m_operandStack.push_back(m_builder.getInt32(integerInfo->value)); },
                [&](const FloatInfo* floatInfo)
                { m_operandStack.push_back(llvm::ConstantFP::get(m_builder.getFloatTy(), floatInfo->value)); },
                [&](const LongInfo* longInfo) { m_operandStack.push_back(m_builder.getInt64(longInfo->value)); },
                [&](const DoubleInfo* doubleInfo)
                { m_operandStack.push_back(llvm::ConstantFP::get(m_builder.getDoubleTy(), doubleInfo->value)); },
                [&](const StringInfo* stringInfo)
                {
                    llvm::StringRef text = stringInfo->stringValue.resolve(m_classFile)->text;

                    String* string = m_stringInterner.intern(text);

                    m_operandStack.push_back(
                        m_builder.CreateIntToPtr(m_builder.getInt64(reinterpret_cast<std::uint64_t>(string)),
                                                 referenceType(m_builder.getContext())));
                },
                [&](const ClassInfo*) { m_operandStack.push_back(loadClassObjectFromPool(ldc.index)); },
                [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
        },
        [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
        {
            llvm::Value* key = m_operandStack.pop_back();

            llvm::BasicBlock* defaultBlock = m_basicBlocks[switchOp.offset + switchOp.defaultOffset];
            m_basicBlockStackStates.insert({defaultBlock, m_operandStack.saveState()});

            auto* switchInst = m_builder.CreateSwitch(key, defaultBlock, switchOp.matchOffsetsPairs.size());

            for (auto [match, target] : switchOp.matchOffsetsPairs)
            {
                llvm::BasicBlock* targetBlock = m_basicBlocks[switchOp.offset + target];
                m_basicBlockStackStates.insert({targetBlock, m_operandStack.saveState()});

                switchInst->addCase(m_builder.getInt32(match), targetBlock);
            }
        },
        [&](OneOf<LShl, LShr, LUShr>)
        {
            llvm::Value* rhs = m_operandStack.pop_back();
            llvm::Value* maskedRhs = m_builder.CreateAnd(
                rhs, m_builder.getInt32(0x3F)); // According to JVM only the lower 6 bits shall be considered
            llvm::Value* extendedRhs = m_builder.CreateSExt(
                maskedRhs,
                m_builder.getInt64Ty()); // LLVM only accepts binary ops with the same types for both operands
            llvm::Value* lhs = m_operandStack.pop_back();

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid shift operation"); },
                [&](LShl) { return m_builder.CreateShl(lhs, extendedRhs); },
                [&](LShr) { return m_builder.CreateAShr(lhs, extendedRhs); },
                [&](LUShr) { return m_builder.CreateLShr(lhs, extendedRhs); });

            m_operandStack.push_back(result);
        },
        [&](OneOf<MonitorEnter, MonitorExit>)
        {
            // Pop object as is required by the instruction.
            // TODO: If we ever care about multi threading, this would require lazily creating a mutex and
            //  (un)locking it.
            m_operandStack.pop_back();
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
                          [&] { return llvm::BasicBlock::Create(m_builder.getContext(), "start", m_function); });

            std::generate(loopEnds.rbegin(), loopEnds.rend(),
                          [&] { return llvm::BasicBlock::Create(m_builder.getContext(), "end", m_function); });

            std::generate(loopCounts.rbegin(), loopCounts.rend(),
                          // TODO: throw NegativeArraySizeException
                          [&] { return m_operandStack.pop_back(); });

            std::generate(arrayClassObjects.begin(), arrayClassObjects.end(),
                          [&]
                          {
                              llvm::Value* classObject = m_helper.getClassObject(m_builder, descriptor);
                              descriptor = descriptor.drop_front();

                              return classObject;
                          });

            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(arrayClassObjects[0]))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::BasicBlock* done = llvm::BasicBlock::Create(m_builder.getContext(), "done", m_function);

            llvm::Value* size = loopCounts[0];
            llvm::Value* array = generateAllocArray(className, arrayClassObjects[0], size);
            llvm::Value* outerArray = array;
            llvm::BasicBlock* nextEnd = done;

            // in C++23: std::ranges::zip_transform_view
            for (int i = 0; i < iterations; i++)
            {
                llvm::BasicBlock* start = loopStarts[i];
                llvm::BasicBlock* end = loopEnds[i];
                llvm::BasicBlock* last = m_builder.GetInsertBlock();

                llvm::Value* innerSize = loopCounts[i + 1];
                llvm::Value* classObject = arrayClassObjects[i + 1];

                llvm::Value* cmp = m_builder.CreateICmpSGT(size, m_builder.getInt32(0));
                m_builder.CreateCondBr(cmp, start, nextEnd);

                m_builder.SetInsertPoint(start);

                llvm::PHINode* phi = m_builder.CreatePHI(m_builder.getInt32Ty(), 2);
                phi->addIncoming(m_builder.getInt32(0), last);

                llvm::Value* innerArray = generateAllocArray(className.drop_front(), classObject, innerSize);

                llvm::Value* gep = m_builder.CreateGEP(arrayStructType(referenceType(m_builder.getContext())),
                                                       outerArray, {m_builder.getInt32(0), m_builder.getInt32(2), phi});
                m_builder.CreateStore(innerArray, gep);

                m_builder.SetInsertPoint(end);

                llvm::Value* counter = m_builder.CreateAdd(phi, m_builder.getInt32(1));
                phi->addIncoming(counter, end);

                cmp = m_builder.CreateICmpEQ(counter, size);
                m_builder.CreateCondBr(cmp, nextEnd, start);

                m_builder.SetInsertPoint(start);
                className = className.drop_front();
                outerArray = innerArray;
                size = innerSize;
                nextEnd = end;
            }

            m_builder.CreateBr(loopEnds.back());
            m_builder.SetInsertPoint(done);

            m_operandStack.push_back(array);
        },
        [&](New newOp)
        {
            llvm::Value* classObject = loadClassObjectFromPool(newOp.index);

            // Size is first 4 bytes in the class object and does not include the object header.
            llvm::Value* fieldAreaPtr = m_builder.CreateGEP(
                m_builder.getInt8Ty(), classObject, {m_builder.getInt32(ClassObject::getFieldAreaSizeOffset())});
            llvm::Value* size = m_builder.CreateLoad(m_builder.getInt32Ty(), fieldAreaPtr);
            size = m_builder.CreateAdd(size, m_builder.getInt32(sizeof(ObjectHeader)));

            llvm::Module* module = m_function->getParent();
            llvm::Value* object = m_builder.CreateCall(allocationFunction(module), size);
            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            // Store object header (which in our case is just the class object) in the object.
            m_builder.CreateStore(classObject, object);
            m_operandStack.push_back(object);
        },
        [&](NewArray newArray)
        {
            auto [descriptor, type, size, elementOffset] = resolveNewArrayInfo(newArray.atype, m_builder);
            // TODO: throw NegativeArraySizeException
            llvm::Value* count = m_operandStack.pop_back();

            llvm::Value* classObject = m_helper.getClassObject(m_builder, "[" + descriptor);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may
            // occur.
            if (!llvm::isa<llvm::Constant>(classObject))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            // Size required is the size of the array prior to the elements (equal to the offset to the
            // elements) plus element count * element size.
            llvm::Value* bytesNeeded = m_builder.getInt32(elementOffset);
            bytesNeeded = m_builder.CreateAdd(bytesNeeded, m_builder.CreateMul(count, m_builder.getInt32(size)));

            // Type object.
            llvm::Value* object = m_builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);

            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            m_builder.CreateStore(classObject, object);
            // Array length.
            llvm::Value* gep =
                m_builder.CreateGEP(arrayStructType(type), object, {m_builder.getInt32(0), m_builder.getInt32(1)});
            m_builder.CreateStore(count, gep);

            m_operandStack.push_back(object);
        },
        [](Nop) {}, [&](Pop) { m_operandStack.pop_back(); },
        [&](Pop2)
        {
            llvm::Type* type = m_operandStack.pop_back_with_type().second;
            if (!isCategoryTwo(type))
            {
                // Form 1: pop two values of a category 1 computational type
                m_operandStack.pop_back();
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
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), m_builder.getContext());
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* objectRef = m_operandStack.pop_back();
            llvm::Value* fieldOffset = m_helper.getInstanceFieldOffset(m_builder, className, fieldName, fieldType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(fieldOffset))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::Value* fieldPtr =
                m_builder.CreateGEP(llvm::Type::getInt8Ty(m_builder.getContext()), objectRef, {fieldOffset});

            if (value->getType() != llvmFieldType)
            {
                // Truncated from the operands stack i32 type.
                assert(value->getType()->isIntegerTy() && llvmFieldType->isIntegerTy()
                       && value->getType()->getIntegerBitWidth() > llvmFieldType->getIntegerBitWidth());
                value = m_builder.CreateTrunc(value, llvmFieldType);
            }

            m_builder.CreateStore(value, fieldPtr);
        },
        [&](PutStatic putStatic)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{putStatic.index}.resolve(m_classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text;
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), m_builder.getContext());
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* fieldPtr = m_helper.getStaticFieldAddress(m_builder, className, fieldName, fieldType);
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
                value = m_builder.CreateTrunc(value, llvmFieldType);
            }

            m_builder.CreateStore(value, fieldPtr);
        },
        // TODO: Ret
        [&](Return) { m_builder.CreateRetVoid(); },
        [&](SIPush siPush) { m_operandStack.push_back(m_builder.getInt32(siPush.value)); },
        [&](Swap)
        {
            llvm::Value* value1 = m_operandStack.pop_back();
            llvm::Value* value2 = m_operandStack.pop_back();

            m_operandStack.push_back(value1);
            m_operandStack.push_back(value2);
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
                    m_builder.CreateStore(m_operandStack.pop_back(), m_locals[wide.index]);
                    return;
                }
                case OpCodes::Ret: llvm_unreachable("NOT YET IMPLEMENTED");
                case OpCodes::IInc:
                {
                    llvm::Value* local = m_builder.CreateLoad(m_builder.getInt32Ty(), m_locals[wide.index]);
                    m_builder.CreateStore(m_builder.CreateAdd(local, m_builder.getInt32(*wide.value)),
                                          m_locals[wide.index]);
                    return;
                }
                case OpCodes::ALoad:
                {
                    type = referenceType(m_builder.getContext());
                    break;
                }
                case OpCodes::DLoad:
                {
                    type = m_builder.getDoubleTy();
                    break;
                }
                case OpCodes::FLoad:
                {
                    type = m_builder.getFloatTy();
                    break;
                }
                case OpCodes::ILoad:
                {
                    type = m_builder.getInt32Ty();
                    break;
                }
                case OpCodes::LLoad:
                {
                    type = m_builder.getInt64Ty();
                    break;
                }
            }

            m_operandStack.push_back(m_builder.CreateLoad(type, m_locals[wide.index]));
        });
}

void CodeGenerator::generateEHDispatch()
{
    llvm::PointerType* referenceTy = referenceType(m_builder.getContext());
    llvm::Value* value = m_builder.CreateLoad(referenceTy, activeException(m_function->getParent()));
    llvm::Value* cond = m_builder.CreateICmpEQ(value, llvm::ConstantPointerNull::get(referenceTy));

    auto* continueBlock = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
    m_builder.CreateCondBr(cond, continueBlock, generateHandlerChain(value, m_builder.GetInsertBlock()));

    m_builder.SetInsertPoint(continueBlock);
}

llvm::BasicBlock* CodeGenerator::generateHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred)
{
    llvm::IRBuilder<>::InsertPointGuard guard{m_builder};

    auto result = m_alreadyGeneratedHandlers.find(m_activeHandlers);
    if (result != m_alreadyGeneratedHandlers.end())
    {
        llvm::BasicBlock* block = result->second;
        // Adding new predecessors exception object to phi node.
        llvm::cast<llvm::PHINode>(&block->front())->addIncoming(exception, newPred);
        return block;
    }

    auto* ehHandler = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
    m_alreadyGeneratedHandlers.emplace(m_activeHandlers, ehHandler);
    m_builder.SetInsertPoint(ehHandler);

    llvm::PHINode* phi = m_builder.CreatePHI(exception->getType(), 0);
    phi->addIncoming(exception, newPred);

    for (auto [handlerPC, catchType] : m_activeHandlers)
    {
        llvm::BasicBlock* handlerBB = m_basicBlocks[handlerPC];

        llvm::PointerType* ty = referenceType(m_builder.getContext());

        if (!catchType)
        {
            // Catch all used to implement 'finally'.
            // Set exception object as only object on the stack and clear the active exception.
            m_builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(m_function->getParent()));
            m_operandStack.setHandlerStack(phi);
            m_builder.CreateBr(handlerBB);
            return ehHandler;
        }

        llvm::SmallString<64> buffer;
        llvm::Value* className = m_builder.CreateGlobalStringPtr(
            ("L" + catchType.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text + ";").toStringRef(buffer));
        // Since an exception class must be loaded for any instance of the class to be created, we can be
        // certain that the exception is not of the type if the class has not yet been loaded. And most
        // importantly, don't need to eagerly load it.
        llvm::Value* classObject = m_builder.CreateCall(forNameLoadedFunction(m_function->getParent()), className);
        llvm::Value* notLoaded = m_builder.CreateICmpEQ(classObject, llvm::ConstantPointerNull::get(ty));

        auto* nextHandler = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
        auto* instanceOfCheck = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
        m_builder.CreateCondBr(notLoaded, nextHandler, instanceOfCheck);

        m_builder.SetInsertPoint(instanceOfCheck);

        llvm::Value* call = m_builder.CreateCall(instanceOfFunction(m_function->getParent()), {phi, classObject});
        call = m_builder.CreateTrunc(call, m_builder.getInt1Ty());

        auto* jumpToHandler = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
        m_builder.CreateCondBr(call, jumpToHandler, nextHandler);

        m_builder.SetInsertPoint(jumpToHandler);
        // Set exception object as only object on the stack and clear the active exception.
        m_operandStack.setHandlerStack(phi);
        m_builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(m_function->getParent()));
        m_builder.CreateBr(handlerBB);

        m_builder.SetInsertPoint(nextHandler);
    }

    // Otherwise, propagate exception to parent frame:

    llvm::Type* retType = m_builder.getCurrentFunctionReturnType();
    if (retType->isVoidTy())
    {
        m_builder.CreateRetVoid();
    }
    else
    {
        m_builder.CreateRet(llvm::UndefValue::get(retType));
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
        return m_helper.getClassObject(m_builder, className);
    }

    return m_helper.getClassObject(m_builder, "L" + className + ";");
}

llvm::Value* CodeGenerator::generateAllocArray(llvm::StringRef descriptor, llvm::Value* classObject, llvm::Value* size)
{
    auto [elementType, elementSize, elementOffset] = match(
        parseFieldType(descriptor.drop_front()),
        [&](BaseType baseType) -> std::tuple<llvm::Type*, std::size_t, std::size_t>
        {
            auto [_, eType, eSize, eOffset] =
                resolveNewArrayInfo(static_cast<ArrayOp::ArrayType>(baseType.getValue()), m_builder);
            return {eType, eSize, eOffset};
        },
        [&](auto) -> std::tuple<llvm::Type*, std::size_t, std::size_t> {
            return {referenceType(m_builder.getContext()), sizeof(Object*), Array<>::arrayElementsOffset()};
        });

    // Size required is the size of the array prior to the elements (equal to the offset to the
    // elements) plus element count * element size.
    llvm::Value* bytesNeeded = m_builder.CreateAdd(m_builder.getInt32(elementOffset),
                                                   m_builder.CreateMul(size, m_builder.getInt32(elementSize)));

    // TODO: Allocation can throw OutOfMemoryException, create EH-dispatch
    llvm::Value* array = m_builder.CreateCall(allocationFunction(m_function->getParent()), bytesNeeded);

    m_builder.CreateStore(classObject, array);

    llvm::Value* gep =
        m_builder.CreateGEP(arrayStructType(elementType), array, {m_builder.getInt32(0), m_builder.getInt32(1)});
    m_builder.CreateStore(size, gep);

    return array;
}
