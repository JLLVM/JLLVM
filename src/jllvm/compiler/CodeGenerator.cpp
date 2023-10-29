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

#include "CodeGenerator.hpp"

#include <llvm/ADT/IntervalTree.h>
#include <llvm/ADT/ScopeExit.h>
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

llvm::Value* extendToStackType(llvm::IRBuilder<>& builder, FieldType type, llvm::Value* value)
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
    std::string_view descriptor;
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

void CodeGenerator::generateBody(const Code& code, PrologueGenFn generatePrologue, std::uint16_t offset)
{
    llvm::DIFile* file = m_debugBuilder.createFile("temp.java", ".");
    llvm::DICompileUnit* cu = m_debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

    llvm::DISubprogram* subprogram =
        m_debugBuilder.createFunction(file, m_function->getName(), "", file, 1,
                                      m_debugBuilder.createSubroutineType(m_debugBuilder.getOrCreateTypeArray({})), 1,
                                      llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    m_function->setSubprogram(subprogram);
    auto onExit = llvm::make_scope_exit([&] { m_debugBuilder.finalizeSubprogram(subprogram); });

    // Dummy debug location until we generate proper debug location. This is required by LLVM as it requires any call
    // to a function that has debug info and is eligible to be inlined to have debug locations on the call.
    // This is currently the case for self-recursive functions.
    m_builder.SetCurrentDebugLocation(llvm::DILocation::get(m_builder.getContext(), 1, 1, subprogram));

    // We need pointer size bytes, since that is the largest type we may store in a local.
    std::generate(m_locals.begin(), m_locals.end(), [&] { return m_builder.CreateAlloca(m_builder.getPtrTy()); });

    // Perform the type check as the information is potentially required in the prologue generation.
    ByteCodeTypeInfo typeInfo;
    typeInfo.offset = offset;
    ByteCodeTypeChecker checker{m_builder.getContext(), m_classFile, code, m_functionMethodType, typeInfo};

    generatePrologue(m_builder, m_locals, m_operandStack, typeInfo);

    createBasicBlocks(checker);
    // If no basic block exists for the offset compilation is started at, create it. This effectively splits the basic
    // block that the offset is contained in and allows the entry block of this function and the instructions prior to
    // offset to jump to the basic block corresponding to 'offset'.
    auto [iter, inserted] = m_basicBlocks.insert({offset, {}});
    if (inserted)
    {
        iter->second.block = llvm::BasicBlock::Create(m_builder.getContext(), "", m_function);
        iter->second.block->moveAfter(&m_function->getEntryBlock());
        iter->second.state.resize(typeInfo.operandStack.size());
        llvm::transform(typeInfo.operandStack, iter->second.state.begin(), [&](ByteCodeTypeChecker::JVMType type)
                        { return type.is<llvm::Type*>() ? type.get<llvm::Type*>() : m_builder.getPtrTy(); });
    }

    generateCodeBody(code, offset);

    // 'createBasicBlocks' conservatively creates all basic blocks of the code even if some are not reachable if
    // 'offset' is not 0. Delete these basic blocks by detecting them having never been inserted into.
    for (const BasicBlockData& basicBlockData : llvm::make_second_range(m_basicBlocks))
    {
        if (basicBlockData.block->empty())
        {
            basicBlockData.block->eraseFromParent();
        }
    }
}

void CodeGenerator::createBasicBlocks(const ByteCodeTypeChecker& checker)
{
    for (const auto& [offset, state] : checker.getBasicBlocks())
    {
        OperandStack::State stack{state.size()};

        llvm::transform(state, stack.begin(),
                        [&](ByteCodeTypeChecker::JVMType type)
                        { return type.is<llvm::Type*>() ? type.get<llvm::Type*>() : m_builder.getPtrTy(); });

        m_basicBlocks.insert(
            {offset, {llvm::BasicBlock::Create(m_builder.getContext(), "", m_function), std::move(stack)}});
    }

    m_retToMap = checker.makeRetToMap();
}

void CodeGenerator::generateCodeBody(const Code& code, std::uint16_t startOffset)
{
    // IntervalTree used to find all active exception handlers at a given offset. Due to the value type needing to be a
    // primitive type, an index into the exception table is used.
    using IntervalTree = llvm::IntervalTree<std::uint16_t, std::size_t>;
    IntervalTree::Allocator allocator;
    IntervalTree intervalTree(allocator);

    llvm::DenseMap<std::uint16_t, std::vector<Code::ExceptionTable>> startHandlers;
    for (auto&& [index, iter] : llvm::enumerate(code.getExceptionTable()))
    {
        if (iter.startPc == iter.endPc)
        {
            continue;
        }
        startHandlers[iter.startPc].push_back(iter);
        // The interval tree is inclusive while the exception table is exclusive.
        intervalTree.insert(iter.startPc, iter.endPc - 1, index);
    }
    intervalTree.create();

    // Branch from the entry block to the first basic block implementing JVM bytecode.
    m_builder.CreateBr(m_basicBlocks.find(startOffset)->second.block);

    // Loop implementing compilation of at least one basic block. A worklist is used to enqueue all basic blocks that
    // require compilation as discovered during compilation. The inner loop implements compilation of at least one
    // basic block but will fall through and start compiling basic blocks afterwards if that code is an immediate
    // successor of the current block. This is an optimization reducing the amount of times the active exception
    // handlers have to be constructed and the type stack explicitly set.
    m_workList.insert(startOffset);
    while (!m_workList.empty())
    {
        std::uint16_t start = m_workList.pop_back_val();
        {
            auto result = m_basicBlocks.find(start);
            assert(result != m_basicBlocks.end());
            llvm::BasicBlock* block = result->second.block;
            // If the block already has a terminator, then it has been compiled previously and there is nothing to do.
            if (block->getTerminator())
            {
                continue;
            }
            // Move the block after the one that was compiled last to make the basic block order more akin to the
            // order of instructions in bytecode.
            block->moveAfter(m_builder.GetInsertBlock());
            m_builder.SetInsertPoint(block);
            m_operandStack.setState(result->second.state);
        }

        // Compute the exception handlers active right before the new offset.
        m_activeHandlers.clear();
        llvm::DenseMap<std::uint16_t, std::vector<std::list<HandlerInfo>::iterator>> endHandlers;
        if (start != 0 && !intervalTree.empty())
        {
            llvm::SmallVector<std::size_t> handlerIndices = llvm::to_vector(
                llvm::map_range(intervalTree.getContaining(start - 1),
                                [](const IntervalTree::DataType* pointer) { return pointer->value(); }));
            // Order of the entries is the exception table is significant as earlier entries are handled first. Sorting
            // the indices restores this order.
            llvm::sort(handlerIndices);
            for (std::size_t index : handlerIndices)
            {
                const Code::ExceptionTable& entry = code.getExceptionTable()[index];
                m_activeHandlers.emplace_back(entry.handlerPc, entry.catchType);
                endHandlers[entry.endPc].push_back(std::prev(m_activeHandlers.end()));
            }
        }

        llvm::ArrayRef<char> bytes = code.getCode();
        for (auto curr = ByteCodeIterator(bytes.drop_front(start).begin(), start), end = ByteCodeIterator(bytes.end());
             curr != end;)
        {
            ByteCodeOp operation = *curr;
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
            }

            // Break out of the current straight-line code if the instruction does not fallthrough.
            if (!generateInstruction(std::move(operation)))
            {
                break;
            }

            ++curr;

            if (curr == end)
            {
                break;
            }

            // Check if the instruction afterward is part of a new basic block whose insertion point may have to be
            // set.
            auto result = m_basicBlocks.find(curr.getOffset());
            if (result == m_basicBlocks.end())
            {
                continue;
            }

            llvm::BasicBlock* nextBlock = result->second.block;
            if (!m_builder.GetInsertBlock()->getTerminator())
            {
                // If the last instruction of the previous block is not a terminator, then implement implicit
                // fall-through by branching to the basic block right after.
                m_builder.CreateBr(nextBlock);
            }
            // Break out of the straight-line compilation if the next basic block was already compiled.
            if (nextBlock->getTerminator())
            {
                break;
            }
            nextBlock->moveAfter(m_builder.GetInsertBlock());
            m_builder.SetInsertPoint(nextBlock);
        }
    }
}

bool CodeGenerator::generateInstruction(ByteCodeOp operation)
{
    bool fallsThrough = true;

    auto generateRet = [&](auto& ret)
    {
        llvm::Value* retAddress = m_builder.CreateLoad(m_builder.getPtrTy(), m_locals[ret.index]);
        auto& retLocations = m_retToMap[ret.offset];
        auto* indirectBr = m_builder.CreateIndirectBr(retAddress, retLocations.size());
        for (auto location : retLocations)
        {
            indirectBr->addDestination(m_basicBlocks[location].block);
        }
        fallsThrough = false;
    };

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
            llvm::Value* array = m_operandStack.pop_back();

            generateNullPointerCheck(array);

            generateArrayIndexCheck(array, index);

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
            llvm::Value* array = m_operandStack.pop_back();

            generateNullPointerCheck(array);

            generateArrayIndexCheck(array, index);

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
            llvm::Value* count = m_operandStack.pop_back();

            llvm::Value* classObject = getClassObject(
                m_builder, ArrayType(ObjectType(index.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text)));

            generateNegativeArraySizeCheck(count);

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
                    if (m_functionMethodType.returnType() == BaseType(BaseType::Boolean))
                    {
                        value = m_builder.CreateAnd(value, m_builder.getInt32(1));
                    }
                    if (m_function->getReturnType() != value->getType())
                    {
                        value = m_builder.CreateTrunc(value, m_function->getReturnType());
                    }
                });

            m_builder.CreateRet(value);
            fallsThrough = false;
        },
        [&](ArrayLength)
        {
            llvm::Value* array = m_operandStack.pop_back();

            generateNullPointerCheck(array);

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

            generateNullPointerCheck(exception);

            m_builder.CreateStore(exception, activeException(m_function->getParent()));

            m_builder.CreateBr(generateHandlerChain(exception, m_builder.GetInsertBlock()));
            fallsThrough = false;
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
            FieldType descriptor(nameAndTypeInfo->descriptorIndex.resolve(m_classFile)->text);
            llvm::Type* type = descriptorToType(descriptor, m_builder.getContext());

            llvm::Value* objectRef = m_operandStack.pop_back();

            generateNullPointerCheck(objectRef);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            FieldType fieldType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);
            llvm::Value* fieldOffset = getInstanceFieldOffset(m_builder, className, fieldName, fieldType);

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

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
            FieldType fieldType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            llvm::Value* fieldPtr = getStaticFieldAddress(m_builder, className, fieldName, fieldType);

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

            llvm::Type* type = descriptorToType(fieldType, m_builder.getContext());
            llvm::Value* field = m_builder.CreateLoad(type, fieldPtr);

            m_operandStack.push_back(extendToStackType(m_builder, fieldType, field));
        },
        [&](OneOf<Goto, GotoW> gotoOp)
        {
            m_builder.CreateBr(getBasicBlock(gotoOp.offset + gotoOp.target));
            fallsThrough = false;
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
            llvm::BasicBlock* target = getBasicBlock(cmpOp.offset + cmpOp.target);
            llvm::BasicBlock* next = getBasicBlock(cmpOp.offset + sizeof(OpCodes) + sizeof(std::int16_t));

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
            m_builder.CreateCondBr(cond, target, next);
        },
        [&](IInc iInc)
        {
            llvm::Value* local = m_builder.CreateLoad(m_builder.getInt32Ty(), m_locals[iInc.index]);
            m_builder.CreateStore(m_builder.CreateAdd(local, m_builder.getInt32(iInc.byte)), m_locals[iInc.index]);
        },
        // TODO: InvokeDynamic
        [&](OneOf<InvokeInterface, InvokeSpecial, InvokeVirtual> invoke)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(m_classFile);

            MethodType descriptor(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            std::vector<llvm::Value*> args(descriptor.size() + 1);
            for (auto& iter : llvm::reverse(args))
            {
                iter = m_operandStack.pop_back();
            }

            generateNullPointerCheck(args[0]);

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            MethodType methodType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            llvm::FunctionType* functionType = descriptorToType(descriptor, false, m_builder.getContext());
            prepareArgumentsForCall(m_builder, args, functionType);

            llvm::Value* call;
            if (holds_alternative<InvokeSpecial>(operation))
            {
                call = doSpecialCall(m_builder, className, methodName, methodType, args);
            }
            else
            {
                call = doInstanceCall(m_builder, className, methodName, methodType, args,
                                      match(
                                          operation, [](...) -> MethodResolution { llvm_unreachable("unexpected op"); },
                                          [](InvokeInterface) { return MethodResolution::Interface; },
                                          [](InvokeVirtual) { return MethodResolution::Virtual; }));
            }
            generateEHDispatch();

            if (descriptor.returnType() != BaseType(BaseType::Void))
            {
                m_operandStack.push_back(extendToStackType(m_builder, descriptor.returnType(), call));
            }
        },
        [&](InvokeStatic invoke)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(m_classFile);

            MethodType descriptor(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            std::vector<llvm::Value*> args(descriptor.size());
            for (auto& iter : llvm::reverse(args))
            {
                iter = m_operandStack.pop_back();
            }

            llvm::StringRef className = refInfo->classIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text;
            MethodType methodType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);

            llvm::FunctionType* functionType = descriptorToType(descriptor, true, m_builder.getContext());
            prepareArgumentsForCall(m_builder, args, functionType);

            llvm::Value* call = doStaticCall(m_builder, className, methodName, methodType, args);
            generateEHDispatch();

            if (descriptor.returnType() != BaseType(BaseType::Void))
            {
                m_operandStack.push_back(extendToStackType(m_builder, descriptor.returnType(), call));
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
        [&](OneOf<JSR, JSRw> jsr)
        {
            llvm::BasicBlock* target = getBasicBlock(jsr.offset + jsr.target);
            std::uint16_t retAddress =
                jsr.offset + sizeof(OpCodes)
                + (holds_alternative<JSRw>(operation) ? sizeof(std::int32_t) : sizeof(std::int16_t));

            if (auto iter = m_basicBlocks.find(retAddress); iter != m_basicBlocks.end())
            {
                m_workList.insert(retAddress);
                m_operandStack.push_back(llvm::BlockAddress::get(iter->second.block));
            }

            m_builder.CreateBr(target);
            fallsThrough = false;
        },
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

            llvm::BasicBlock* defaultBlock = getBasicBlock(switchOp.offset + switchOp.defaultOffset);

            auto* switchInst = m_builder.CreateSwitch(key, defaultBlock, switchOp.matchOffsetsPairs.size());

            for (auto [match, target] : switchOp.matchOffsetsPairs)
            {
                llvm::BasicBlock* targetBlock = getBasicBlock(switchOp.offset + target);

                switchInst->addCase(m_builder.getInt32(match), targetBlock);
            }
            fallsThrough = false;
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
            generateNullPointerCheck(m_operandStack.pop_back());
        },
        [&](MultiANewArray multiANewArray)
        {
            auto descriptor = get<ArrayType>(FieldType(
                PoolIndex<ClassInfo>{multiANewArray.index}.resolve(m_classFile)->nameIndex.resolve(m_classFile)->text));

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

            std::generate(loopCounts.rbegin(), loopCounts.rend(), [&] { return m_operandStack.pop_back(); });

            {
                FieldType copy = descriptor;
                std::generate(arrayClassObjects.begin(), arrayClassObjects.end(),
                              [&]
                              {
                                  llvm::Value* classObject = getClassObject(m_builder, copy);
                                  copy = get<ArrayType>(copy).getComponentType();

                                  return classObject;
                              });
            }

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

            llvm::for_each(loopCounts, [&](llvm::Value* count) { generateNegativeArraySizeCheck(count); });

            llvm::BasicBlock* done = llvm::BasicBlock::Create(m_builder.getContext(), "done", m_function);

            llvm::Value* size = loopCounts[0];
            llvm::Value* array = generateAllocArray(descriptor, arrayClassObjects[0], size);
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

                llvm::Value* innerArray =
                    generateAllocArray(get<ArrayType>(descriptor.getComponentType()), classObject, innerSize);

                llvm::Value* gep = m_builder.CreateGEP(arrayStructType(referenceType(m_builder.getContext())),
                                                       outerArray, {m_builder.getInt32(0), m_builder.getInt32(2), phi});
                m_builder.CreateStore(innerArray, gep);

                m_builder.SetInsertPoint(end);

                llvm::Value* counter = m_builder.CreateAdd(phi, m_builder.getInt32(1));
                phi->addIncoming(counter, end);

                cmp = m_builder.CreateICmpEQ(counter, size);
                m_builder.CreateCondBr(cmp, nextEnd, start);

                m_builder.SetInsertPoint(start);
                descriptor = get<ArrayType>(descriptor.getComponentType());
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
            llvm::Value* count = m_operandStack.pop_back();

            llvm::Value* classObject = getClassObject(m_builder, ArrayType(descriptor));

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

            generateNegativeArraySizeCheck(count);

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
            FieldType fieldType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);
            llvm::Type* llvmFieldType = descriptorToType(fieldType, m_builder.getContext());
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* objectRef = m_operandStack.pop_back();

            generateNullPointerCheck(objectRef);

            llvm::Value* fieldOffset = getInstanceFieldOffset(m_builder, className, fieldName, fieldType);

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

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
            FieldType fieldType(
                refInfo->nameAndTypeIndex.resolve(m_classFile)->descriptorIndex.resolve(m_classFile)->text);
            llvm::Type* llvmFieldType = descriptorToType(fieldType, m_builder.getContext());
            llvm::Value* value = m_operandStack.pop_back();
            llvm::Value* fieldPtr = getStaticFieldAddress(m_builder, className, fieldName, fieldType);

            // Can throw class loader or linkage related errors.
            generateEHDispatch();

            if (value->getType() != llvmFieldType)
            {
                // Truncated from the operands stack i32 type.
                assert(value->getType()->isIntegerTy() && llvmFieldType->isIntegerTy()
                       && value->getType()->getIntegerBitWidth() > llvmFieldType->getIntegerBitWidth());
                value = m_builder.CreateTrunc(value, llvmFieldType);
            }

            m_builder.CreateStore(value, fieldPtr);
        },
        [&](Ret ret) { generateRet(ret); },
        [&](Return)
        {
            m_builder.CreateRetVoid();
            fallsThrough = false;
        },
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
                case OpCodes::Ret:
                {
                    generateRet(wide);
                    return;
                }
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

    return fallsThrough;
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

void CodeGenerator::generateBuiltinExceptionThrow(llvm::Value* condition, llvm::StringRef builderName,
                                                  llvm::ArrayRef<llvm::Value*> builderArgs)
{
    llvm::PointerType* exceptionType = referenceType(m_builder.getContext());

    auto* continueBlock = llvm::BasicBlock::Create(m_builder.getContext(), "next", m_function);
    auto* exceptionBlock = llvm::BasicBlock::Create(m_builder.getContext(), "exception", m_function);
    m_builder.CreateCondBr(condition, exceptionBlock, continueBlock);
    m_builder.SetInsertPoint(exceptionBlock);

    std::vector<llvm::Type*> argTypes{builderArgs.size()};

    llvm::transform(builderArgs, argTypes.begin(), [](llvm::Value* arg) { return arg->getType(); });

    llvm::Value* exception =
        m_builder.CreateCall(m_function->getParent()->getOrInsertFunction(
                                 builderName, llvm::FunctionType::get(exceptionType, argTypes, false)),
                             builderArgs);

    m_builder.CreateStore(exception, activeException(m_function->getParent()));

    m_builder.CreateBr(generateHandlerChain(exception, m_builder.GetInsertBlock()));

    m_builder.SetInsertPoint(continueBlock);
}

void CodeGenerator::generateNullPointerCheck(llvm::Value* object)
{
    llvm::Value* null = llvm::ConstantPointerNull::get(referenceType(m_builder.getContext()));
    llvm::Value* isNull = m_builder.CreateICmpEQ(object, null);

    generateBuiltinExceptionThrow(isNull, "jllvm_build_null_pointer_exception", {});
}

void CodeGenerator::generateArrayIndexCheck(llvm::Value* array, llvm::Value* index)
{
    // The element type of the array type here is actually irrelevant.
    llvm::PointerType* type = referenceType(m_builder.getContext());
    llvm::Value* gep =
        m_builder.CreateGEP(arrayStructType(type), array, {m_builder.getInt32(0), m_builder.getInt32(1)});
    llvm::Value* size = m_builder.CreateLoad(m_builder.getInt32Ty(), gep);

    llvm::Value* isNegative = m_builder.CreateICmpSLT(index, m_builder.getInt32(0));
    llvm::Value* isBigger = m_builder.CreateICmpSGE(index, size);
    llvm::Value* outOfBounds = m_builder.CreateOr(isNegative, isBigger);

    generateBuiltinExceptionThrow(outOfBounds, "jllvm_build_array_index_out_of_bounds_exception", {index, size});
}

void CodeGenerator::generateNegativeArraySizeCheck(llvm::Value* size)
{
    llvm::Value* isNegative = m_builder.CreateICmpSLT(size, m_builder.getInt32(0));

    generateBuiltinExceptionThrow(isNegative, "jllvm_build_negative_array_size_exception", {size});
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
        llvm::BasicBlock* handlerBB = getBasicBlock(handlerPC);

        llvm::PointerType* ty = referenceType(m_builder.getContext());

        if (!catchType)
        {
            // Catch all used to implement 'finally'.
            // Set exception object as only object on the stack and clear the active exception.
            m_builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(m_function->getParent()));
            m_operandStack.setBottomOfStackValue(phi);
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
        m_operandStack.setBottomOfStackValue(phi);
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
        return getClassObject(m_builder, FieldType(className));
    }

    return getClassObject(m_builder, ObjectType(className));
}

llvm::Value* CodeGenerator::generateAllocArray(ArrayType descriptor, llvm::Value* classObject, llvm::Value* size)
{
    auto [elementType, elementSize, elementOffset] = match(
        descriptor.getComponentType(),
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

llvm::Value* CodeGenerator::doStaticCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                         llvm::StringRef methodName, MethodType methodType,
                                         llvm::ArrayRef<llvm::Value*> args)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionType* functionType = descriptorToType(methodType, /*isStatic=*/true, builder.getContext());
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleStaticCall(className, methodName, methodType), functionType);
    applyABIAttributes(llvm::cast<llvm::Function>(function.getCallee()), methodType, /*isStatic=*/true);
    llvm::CallInst* call = builder.CreateCall(function, args);
    applyABIAttributes(call, methodType, /*isStatic=*/true);
    return call;
}

llvm::Value* CodeGenerator::doInstanceCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                           llvm::StringRef methodName, MethodType methodType,
                                           llvm::ArrayRef<llvm::Value*> args, MethodResolution resolution)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionType* functionType = descriptorToType(methodType, /*isStatic=*/false, builder.getContext());
    llvm::FunctionCallee function = module->getOrInsertFunction(
        mangleMethodResolutionCall(resolution, className, methodName, methodType), functionType);
    applyABIAttributes(llvm::cast<llvm::Function>(function.getCallee()), methodType, /*isStatic=*/false);
    llvm::CallInst* call = builder.CreateCall(function, args);
    applyABIAttributes(call, methodType, /*isStatic=*/false);
    return call;
}

llvm::Value* CodeGenerator::doSpecialCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                          llvm::StringRef methodName, MethodType methodType,
                                          llvm::ArrayRef<llvm::Value*> args)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionType* functionType = descriptorToType(methodType, /*isStatic=*/false, builder.getContext());
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleSpecialMethodCall(className, methodName, methodType,
                                                            m_classFile.hasSuperFlag() ? m_classObject.getDescriptor() :
                                                                                         std::optional<FieldType>{}),
                                    functionType);
    applyABIAttributes(llvm::cast<llvm::Function>(function.getCallee()), methodType, /*isStatic=*/false);
    llvm::CallInst* call = builder.CreateCall(function, args);
    applyABIAttributes(call, methodType, /*isStatic=*/false);
    return call;
}

llvm::Value* CodeGenerator::getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                   llvm::StringRef fieldName, FieldType fieldType)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleFieldAccess(className, fieldName, fieldType),
                                    llvm::FunctionType::get(builder.getIntNTy(sizeof(std::size_t) * 8), false));
    return builder.CreateCall(function);
}

llvm::Value* CodeGenerator::getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                  llvm::StringRef fieldName, FieldType fieldType)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleFieldAccess(className, fieldName, fieldType),
                                    llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), false));
    return builder.CreateCall(function);
}

llvm::Value* CodeGenerator::getClassObject(llvm::IRBuilder<>& builder, FieldType fieldDescriptor)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function = module->getOrInsertFunction(
        mangleClassObjectAccess(fieldDescriptor), llvm::FunctionType::get(referenceType(builder.getContext()), false));
    return builder.CreateCall(function);
}
