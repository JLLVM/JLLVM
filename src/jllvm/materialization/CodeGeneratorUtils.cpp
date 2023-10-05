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

#include "CodeGeneratorUtils.hpp"

#include <llvm/IR/DIBuilder.h>

#include "ByteCodeLayer.hpp"
#include "ClassObjectStubMangling.hpp"
#include "LambdaMaterialization.hpp"

using namespace jllvm;

namespace
{
/// X86 ABI essentially always uses the 32 bit register names for passing along integers. Using the 'signext' and
/// 'zeroext' attribute we tell LLVM that if due to ABI, it has to extend these registers, which extension to use.
/// This attribute list can be applied to either a call or a function itself.
llvm::AttributeList getABIAttributes(llvm::LLVMContext& context, MethodType methodType, bool isStatic)
{
    llvm::SmallVector<llvm::AttributeSet> paramAttrs(methodType.size());
    for (auto&& [param, attrs] : llvm::zip(methodType.parameters(), paramAttrs))
    {
        auto baseType = get_if<BaseType>(&param);
        if (!baseType || !baseType->isIntegerType())
        {
            continue;
        }
        attrs = attrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }

    llvm::AttributeSet retAttrs;
    FieldType returnType = methodType.returnType();
    if (auto baseType = get_if<BaseType>(&returnType); baseType && baseType->isIntegerType())
    {
        retAttrs =
            retAttrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }
    if (!isStatic)
    {
        paramAttrs.insert(paramAttrs.begin(), llvm::AttributeSet().addAttribute(context, llvm::Attribute::NonNull));
    }
    return llvm::AttributeList::get(context, llvm::AttributeSet{}, retAttrs, paramAttrs);
}

class TrivialDebugInfoBuilder
{
    llvm::DIBuilder m_debugBuilder;
    llvm::DISubprogram* m_subProgram;

public:
    TrivialDebugInfoBuilder(llvm::Function* function) : m_debugBuilder(*function->getParent())
    {
        llvm::DIFile* file = m_debugBuilder.createFile(".", ".");
        m_debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

        m_subProgram =
            m_debugBuilder.createFunction(file, function->getName(), "", file, 1,
                                          m_debugBuilder.createSubroutineType(m_debugBuilder.getOrCreateTypeArray({})),
                                          1, llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);

        function->setSubprogram(m_subProgram);
    }

    ~TrivialDebugInfoBuilder()
    {
        finalize();
    }

    TrivialDebugInfoBuilder(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder(TrivialDebugInfoBuilder&&) = delete;
    TrivialDebugInfoBuilder& operator=(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder& operator=(TrivialDebugInfoBuilder&&) = delete;

    void finalize()
    {
        if (!m_subProgram)
        {
            return;
        }
        m_debugBuilder.finalizeSubprogram(std::exchange(m_subProgram, nullptr));
        m_debugBuilder.finalize();
    }
};

bool isCategoryTwo(ByteCodeTypeChecker::JVMType type)
{
    return type.is<llvm::Type*>()
           && (type.get<llvm::Type*>()->isIntegerTy(64) || type.get<llvm::Type*>()->isDoubleTy());
}

template <class... Args>
struct OneOfBase : ByteCodeBase
{
    template <class T, class = std::enable_if_t<(std::is_same_v<std::decay_t<T>, Args> || ...)>>
    OneOfBase(T&& value) : ByteCodeBase(std::forward<T>(value))
    {
    }
};
} // namespace

void ByteCodeTypeChecker::checkBasicBlock(llvm::ArrayRef<char> block, std::uint16_t offset, TypeStack typeStack)
{
    bool done = false;

    auto pushNext = [&](std::uint16_t next)
    {
        if (m_basicBlocks.insert({next, typeStack}).second)
        {
            m_offsetStack.push_back(next);
        }
    };
    auto checkRet = [&](auto& ret)
    {
        std::uint16_t retAddress = m_localRetMap[ret.index];
        m_subroutineToReturnInfoMap.insert(
            {m_returnAddressToSubroutineMap[retAddress], {static_cast<std::uint16_t>(ret.offset), retAddress}});

        pushNext(retAddress);
        done = true;
    };
    auto checkAstore = [&](auto& aStore)
    {
        JVMType type = typeStack.back();
        typeStack.pop_back();

        if (type.is<RetAddrType>())
        {
            m_localRetMap[aStore.index] = type.get<RetAddrType>();
        }
    };

    for (ByteCodeOp operation : byteCodeRange(block, offset))
    {
        if (done)
        {
            return;
        }

        match(
            operation, [](...) { llvm_unreachable("NOT YET IMPLEMENTED"); },
            [&](OneOfBase<AALoad, ANewArray, NewArray>)
            {
                if (holds_alternative<AALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_addressType;
            },
            [&](OneOfBase<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
            { typeStack.erase(typeStack.end() - 3, typeStack.end()); },
            [&](OneOfBase<AConstNull, ALoad, ALoad0, ALoad1, ALoad2, ALoad3, New>)
            { typeStack.emplace_back(m_addressType); },
            [&](OneOfBase<AReturn, AThrow, DReturn, FReturn, IReturn, LReturn, Return>) { done = true; },
            [&](AStore aStore) { checkAstore(aStore); },
            [&](OneOf<AStore0, AStore1, AStore2, AStore3>)
            {
                JVMType type = typeStack.back();
                typeStack.pop_back();

                if (type.is<RetAddrType>())
                {
                    auto index = match(
                        operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                        [&](AStore0) { return 0; }, [&](AStore1) { return 1; }, [&](AStore2) { return 2; },
                        [&](AStore3) { return 3; });
                    m_localRetMap[index] = type.get<RetAddrType>();
                }
            },
            [&](OneOfBase<ArrayLength, D2I, F2I, InstanceOf, L2I>) { typeStack.back() = m_intType; },
            [&](OneOfBase<CheckCast, DNeg, FNeg, I2B, I2C, I2S, IInc, INeg, LNeg, Nop>) { /* Types do not change */ },
            [&](OneOfBase<BALoad, CALoad, DCmpG, DCmpL, FCmpG, FCmpL, IALoad, LCmp, SALoad>)
            {
                typeStack.pop_back();
                typeStack.back() = m_intType;
            },
            [&](OneOfBase<BIPush, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4, IConst5, ILoad, ILoad0, ILoad1,
                          ILoad2, ILoad3, SIPush>) { typeStack.emplace_back(m_intType); },
            [&](OneOfBase<D2F, I2F, L2F, FALoad>)
            {
                if (holds_alternative<FALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_floatType;
            },
            [&](OneOfBase<D2L, F2L, I2L, LALoad>)
            {
                if (holds_alternative<LALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_longType;
            },
            [&](OneOfBase<DAdd, DDiv, DMul, DRem, DStore, DStore0, DStore1, DStore2, DStore3, DSub, FAdd, FDiv, FMul,
                          FRem, FStore, FStore0, FStore1, FStore2, FStore3, FSub, IAdd, IAnd, IDiv, IMul, IOr, IRem,
                          IShl, IShr, IStore, IStore0, IStore1, IStore2, IStore3, ISub, IUShr, IXor, LAdd, LAnd, LDiv,
                          LMul, LOr, LRem, LShl, LShr, LStore, LStore0, LStore1, LStore2, LStore3, LSub, LUShr, LXor,
                          MonitorEnter, MonitorExit, Pop, PutStatic>) { typeStack.pop_back(); },
            [&](OneOfBase<DALoad, F2D, I2D, L2D>)
            {
                if (holds_alternative<DALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_doubleType;
            },
            [&](OneOfBase<DConst0, DConst1, DLoad, DLoad0, DLoad1, DLoad2, DLoad3>)
            { typeStack.emplace_back(m_doubleType); },
            [&](Dup) { typeStack.push_back(typeStack.back()); },
            [&](DupX1)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                assert(!isCategoryTwo(type1) && !isCategoryTwo(type2));

                typeStack.insert(iter.base(), type1);
            },
            [&](DupX2)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type2))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                    ++iter;
                }

                typeStack.insert(iter.base(), type1);
            },
            [&](Dup2)
            {
                auto iter = typeStack.rbegin();
                JVMType type = *iter++;

                if (!isCategoryTwo(type))
                {
                    // Form 1: where both value1 and value2 are values of a category 1 computational type
                    JVMType type2 = *iter++;

                    typeStack.push_back(type2);
                }

                typeStack.push_back(type);
            },
            [&](Dup2X1)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type

                    typeStack.insert((++iter).base(), {type2, type1});
                }
                else
                {
                    // Form 2: where value1 is a value of a category 2 computational type and value2 is a value of a
                    // category 1 computational type
                    typeStack.insert(iter.base(), type1);
                }
            },
            [&](Dup2X2)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    JVMType type3 = *iter++;

                    if (!isCategoryTwo(type3))
                    {
                        // Form 1: where value1, value2, value3, and value4 are all values of a category 1 computational
                        // type
                        ++iter;
                    }

                    typeStack.insert(iter.base(), {type2, type1});
                }
                else
                {
                    if (!isCategoryTwo(type2))
                    {
                        // Form 2: where value1 is a value of a category 2 computational type and value2 and value3 are
                        // both values of a category 1 computational type
                        ++iter;
                    }

                    typeStack.insert(iter.base(), type1);
                }
            },
            [&](OneOfBase<FConst0, FConst1, FConst2, FLoad, FLoad0, FLoad1, FLoad2, FLoad3>)
            { typeStack.emplace_back(m_floatType); },
            [&](OneOf<GetField, GetStatic> get)
            {
                if (holds_alternative<GetField>(operation))
                {
                    typeStack.pop_back();
                }

                auto descriptor = FieldType(PoolIndex<FieldRefInfo>{get.index}
                                                .resolve(m_classFile)
                                                ->nameAndTypeIndex.resolve(m_classFile)
                                                ->descriptorIndex.resolve(m_classFile)
                                                ->text);

                llvm::Type* type = descriptorToType(descriptor, m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                typeStack.emplace_back(type);
            },
            [&](OneOf<Goto, GotoW> gotoOp)
            {
                pushNext(gotoOp.offset + gotoOp.target);
                done = true;
            },
            [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                      IfGe, IfGt, IfLe, IfNonNull, IfNull>
                    cmpOp)
            {
                typeStack.pop_back();

                match(
                    operation,
                    [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                    { typeStack.pop_back(); },
                    [](...) {});

                pushNext(cmpOp.offset + cmpOp.target);
                pushNext(cmpOp.offset + sizeof(OpCodes) + sizeof(std::int16_t));
                done = true;
            },
            // TODO InvokeDynamic
            [&](OneOf<InvokeInterface, InvokeSpecial, InvokeStatic, InvokeVirtual> invoke)
            {
                auto descriptor = MethodType(PoolIndex<RefInfo>{invoke.index}
                                                 .resolve(m_classFile)
                                                 ->nameAndTypeIndex.resolve(m_classFile)
                                                 ->descriptorIndex.resolve(m_classFile)
                                                 ->text);

                for (std::size_t i = 0; i < descriptor.size(); i++)
                {
                    typeStack.pop_back();
                }

                // static does not pop this
                if (!holds_alternative<InvokeStatic>(operation))
                {
                    typeStack.pop_back();
                }

                llvm::Type* type = descriptorToType(descriptor.returnType(), m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                if (!type->isVoidTy())
                {
                    typeStack.emplace_back(type);
                }
            },
            [&](OneOf<JSR, JSRw> jsr)
            {
                std::uint16_t retAddress =
                    jsr.offset + sizeof(OpCodes)
                    + (holds_alternative<JSRw>(operation) ? sizeof(std::int32_t) : sizeof(std::int16_t));
                std::uint16_t target = jsr.offset + jsr.target;

                m_returnAddressToSubroutineMap.insert({retAddress, target});

                // check if the subroutine has already been type-checked. If so use the previously calculated typeStack
                if (auto iter = m_subroutineToReturnInfoMap.find(target); iter != m_subroutineToReturnInfoMap.end())
                {
                    typeStack = m_basicBlocks[iter->second.returnAddress];
                    pushNext(retAddress);
                }
                else
                {
                    typeStack.emplace_back(retAddress);
                    pushNext(target);
                }

                done = true;
            },
            [&](OneOfBase<LConst0, LConst1, LLoad, LLoad0, LLoad1, LLoad2, LLoad3>)
            { typeStack.emplace_back(m_longType); },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(m_classFile), [&](const ClassInfo*) { typeStack.emplace_back(m_addressType); },
                    [&](const DoubleInfo*) { typeStack.emplace_back(m_doubleType); },
                    [&](const FloatInfo*) { typeStack.emplace_back(m_floatType); },
                    [&](const IntegerInfo*) { typeStack.emplace_back(m_intType); },
                    [&](const LongInfo*) { typeStack.emplace_back(m_longType); },
                    [&](const StringInfo*) { typeStack.emplace_back(m_addressType); },
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
            },
            [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
            {
                typeStack.pop_back();

                pushNext(switchOp.offset + switchOp.defaultOffset);

                for (std::int32_t target : llvm::make_second_range(switchOp.matchOffsetsPairs))
                {
                    pushNext(switchOp.offset + target);
                }
                done = true;
            },
            [&](MultiANewArray multiANewArray)
            {
                for (int i = 0; i < multiANewArray.dimensions; ++i)
                {
                    typeStack.pop_back();
                }

                typeStack.emplace_back(m_addressType);
            },
            [&](Pop2)
            {
                JVMType type = typeStack.back();
                typeStack.pop_back();
                if (!isCategoryTwo(type))
                {
                    typeStack.pop_back();
                }
            },
            [&](PutField)
            {
                typeStack.pop_back();
                typeStack.pop_back();
            },
            [&](Ret ret) { checkRet(ret); }, [&](Swap) { std::swap(typeStack.back(), *std::next(typeStack.rbegin())); },
            [&](Wide wide)
            {
                llvm::Type* type;
                switch (wide.opCode)
                {
                    default: llvm_unreachable("Invalid wide operation");
                    case OpCodes::AStore:
                    {
                        checkAstore(wide);
                        return;
                    }
                    case OpCodes::DStore:
                    case OpCodes::FStore:
                    case OpCodes::IStore:
                    case OpCodes::LStore:
                    {
                        typeStack.pop_back();
                        return;
                    }
                    case OpCodes::Ret:
                    {
                        checkRet(wide);
                        return;
                    }
                    case OpCodes::IInc:
                    {
                        return;
                    }
                    case OpCodes::ALoad:
                    {
                        type = m_addressType;
                        break;
                    }
                    case OpCodes::DLoad:
                    {
                        type = m_doubleType;
                        break;
                    }
                    case OpCodes::FLoad:
                    {
                        type = m_floatType;
                        break;
                    }
                    case OpCodes::ILoad:
                    {
                        type = m_intType;
                        break;
                    }
                    case OpCodes::LLoad:
                    {
                        type = m_longType;
                        break;
                    }
                }

                typeStack.emplace_back(type);
            });
    }
}

void ByteCodeTypeChecker::check()
{
    for (const auto& exception : m_code.getExceptionTable())
    {
        if (m_basicBlocks.insert({exception.handlerPc, {m_addressType}}).second)
        {
            m_offsetStack.push_back(exception.handlerPc);
        }
    }

    m_basicBlocks.insert({0, {}});
    m_offsetStack.push_back(0);

    while (!m_offsetStack.empty())
    {
        std::uint16_t startOffset = m_offsetStack.back();
        m_offsetStack.pop_back();

        checkBasicBlock(m_code.getCode().drop_front(startOffset), startOffset, m_basicBlocks[startOffset]);
    }
}

ByteCodeTypeChecker::PossibleRetsMap ByteCodeTypeChecker::makeRetToMap()
{
    PossibleRetsMap map;

    for (auto& [returnAddr, subroutine] : m_returnAddressToSubroutineMap)
    {
        map[m_subroutineToReturnInfoMap[subroutine].retOffset].insert(returnAddr);
    }

    return map;
}

void LazyClassLoaderHelper::buildClassInitializerInitStub(llvm::IRBuilder<>& builder, const ClassObject& classObject)
{
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::Module* module = function->getParent();

    llvm::Value* classObjectLLVM =
        builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(&classObject)), builder.getPtrTy());
    auto* initializedGEP =
        builder.CreateGEP(builder.getInt8Ty(), classObjectLLVM, builder.getInt32(ClassObject::getInitializedOffset()));
    auto* initialized =
        builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), initializedGEP), builder.getInt8(0));

    auto* classInitializer = llvm::BasicBlock::Create(builder.getContext(), "", function);
    auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", function);
    builder.CreateCondBr(initialized, continueBlock, classInitializer);

    builder.SetInsertPoint(classInitializer);

    builder.CreateCall(
        module->getOrInsertFunction("jllvm_initialize_class_object", builder.getVoidTy(), classObjectLLVM->getType()),
        classObjectLLVM);

    builder.CreateBr(continueBlock);

    builder.SetInsertPoint(continueBlock);
}

template <class F>
llvm::Value* LazyClassLoaderHelper::returnConstantForClassObject(llvm::IRBuilder<>& builder, FieldType fieldDescriptor,
                                                                 llvm::Twine key, F&& f, bool mustInitializeClassObject)
{
    auto returnValueToIRConstant = [](llvm::IRBuilder<>& builder, const auto& retVal)
    {
        using RetType = std::decay_t<decltype(retVal)>;
        return CppToLLVMType<RetType>::getConstant(retVal, builder);
    };

    if (const ClassObject* classObject = m_classLoader.forNameLoaded(fieldDescriptor))
    {
        if (mustInitializeClassObject && !classObject->isInitialized())
        {
            buildClassInitializerInitStub(builder, *classObject);
        }
        return returnValueToIRConstant(builder, f(classObject));
    }

    std::string stubSymbol = ("Class load " + fieldDescriptor.textual() + key).str();
    if (!m_stubsManager.findStub(stubSymbol, true))
    {
        llvm::cantFail(m_stubsManager.createStub(
            stubSymbol,
            llvm::cantFail(m_callbackManager.getCompileCallback(
                [=, *this]
                {
                    const ClassObject& classObject = m_classLoader.forName(fieldDescriptor);

                    auto context = std::make_unique<llvm::LLVMContext>();
                    auto module = std::make_unique<llvm::Module>(stubSymbol, *context);

                    module->setDataLayout(m_dataLayout);
                    module->setTargetTriple(LLVM_HOST_TRIPLE);

                    auto* functionType = llvm::FunctionType::get(
                        CppToLLVMType<typename llvm::function_traits<std::decay_t<F>>::result_t>::get(context.get()),
                        false);

                    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                                            stubSymbol, module.get());
                    function->addFnAttr(llvm::Attribute::UWTable);
                    function->setGC("coreclr");
                    TrivialDebugInfoBuilder debugInfoBuilder(function);
                    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                    if (mustInitializeClassObject && !classObject.isInitialized())
                    {
                        buildClassInitializerInitStub(builder, classObject);
                    }

                    builder.CreateRet(returnValueToIRConstant(builder, f(&classObject)));

                    debugInfoBuilder.finalize();

                    llvm::cantFail(m_baseLayer.add(m_implDylib,
                                                   llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

                    auto address =
                        llvm::cantFail(m_implDylib.getExecutionSession().lookup({&m_implDylib}, m_interner(stubSymbol)))
                            .getAddress();

                    llvm::cantFail(m_stubsManager.updatePointer(stubSymbol, address));

                    return address;
                })),
            llvm::JITSymbolFlags::Exported));
        llvm::cantFail(m_mainDylib.define(
            llvm::orc::absoluteSymbols({{m_interner(stubSymbol), m_stubsManager.findStub(stubSymbol, true)}})));
    }

    auto* functionType = llvm::FunctionType::get(
        CppToLLVMType<typename llvm::function_traits<std::decay_t<F>>::result_t>::get(&builder.getContext()), false);

    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function = module->getOrInsertFunction(stubSymbol, functionType);
    return builder.CreateCall(function);
}

template <class F>
llvm::Value* LazyClassLoaderHelper::doCallForClassObject(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                         llvm::StringRef methodName, MethodType methodType,
                                                         bool isStatic, llvm::Twine key,
                                                         llvm::ArrayRef<llvm::Value*> args, F&& f)
{
    llvm::FunctionType* functionType = descriptorToType(methodType, isStatic, builder.getContext());

    std::string method = mangleDirectMethodCall(className, methodName, methodType);
    if (const ClassObject* classObject = m_classLoader.forNameLoaded(ObjectType(className)))
    {
        return f(builder, classObject, args);
    }

    // Otherwise we create a stub to call the class loader at runtime and then later replace the stub with the
    // real method.
    std::string stubName = (key + " " + method).str();

    if (!m_stubsManager.findStub(stubName, true))
    {
        // Create the stub if it hasn't yet been created.
        llvm::cantFail(m_stubsManager.createStub(
            stubName,
            llvm::cantFail(m_callbackManager.getCompileCallback(
                [=, *this]
                {
                    const ClassObject& classObject = m_classLoader.forName(ObjectType(className));

                    auto context = std::make_unique<llvm::LLVMContext>();
                    auto module = std::make_unique<llvm::Module>(stubName, *context);

                    module->setDataLayout(m_dataLayout);
                    module->setTargetTriple(LLVM_HOST_TRIPLE);

                    auto* functionType = descriptorToType(methodType, isStatic, *context);

                    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage, stubName,
                                                            module.get());
                    function->addFnAttr(llvm::Attribute::UWTable);
                    function->setGC("coreclr");
                    TrivialDebugInfoBuilder debugInfoBuilder(function);

                    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                    llvm::SmallVector<llvm::Value*> args;
                    for (llvm::Argument& arg : function->args())
                    {
                        args.push_back(&arg);
                    }

                    llvm::Value* result = f(builder, &classObject, args);

                    // Small optimization, if no instructions were generated and its just a call to some address
                    // or function, just point the stub to it instead.
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(result);
                        call && &function->getEntryBlock().front() == result)
                    {
                        if (llvm::Function* callee = call->getCalledFunction())
                        {
                            auto address = llvm::cantFail(m_mainDylib.getExecutionSession().lookup(
                                                              {&m_mainDylib}, m_interner(callee->getName())))
                                               .getAddress();
                            llvm::cantFail(m_stubsManager.updatePointer(stubName, address));
                            return address;
                        }

                        if (auto* constant = llvm::dyn_cast<llvm::ConstantExpr>(call->getCalledOperand());
                            constant && constant->getOpcode() == llvm::Instruction::IntToPtr)
                        {
                            auto address = llvm::cast<llvm::ConstantInt>(constant->getOperand(0))->getZExtValue();
                            llvm::cantFail(m_stubsManager.updatePointer(stubName, address));
                            return address;
                        }
                    }

                    if (builder.getCurrentFunctionReturnType()->isVoidTy())
                    {
                        builder.CreateRetVoid();
                    }
                    else
                    {
                        builder.CreateRet(result);
                    }

                    debugInfoBuilder.finalize();

                    llvm::cantFail(m_baseLayer.add(m_implDylib,
                                                   llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

                    auto address =
                        llvm::cantFail(m_implDylib.getExecutionSession().lookup({&m_implDylib}, m_interner(stubName)))
                            .getAddress();

                    llvm::cantFail(m_stubsManager.updatePointer(stubName, address));

                    return address;
                })),
            llvm::JITSymbolFlags::Exported));

        llvm::cantFail(m_mainDylib.define(
            llvm::orc::absoluteSymbols({{m_interner(stubName), m_stubsManager.findStub(stubName, true)}})));
    }

    llvm::Module* module = builder.GetInsertBlock()->getModule();
    auto* call = builder.CreateCall(module->getOrInsertFunction(stubName, functionType), args);
    call->setAttributes(getABIAttributes(builder.getContext(), methodType, isStatic));
    return call;
}

const jllvm::Method* LazyClassLoaderHelper::methodResolution(const ClassObject* classObject, llvm::StringRef methodName,
                                                             MethodType methodType)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.3

    // Otherwise, method resolution attempts to locate the referenced method
    // in C and its superclasses:

    // Otherwise, if C declares a method with the name and descriptor
    // specified by the method reference, method lookup succeeds.

    // Otherwise, if C has a superclass, step 2 of method resolution is
    // recursively invoked on the direct superclass of C.
    if (const Method* iter = classObject->getMethod(methodName, methodType))
    {
        return iter;
    }

    // Otherwise, method resolution attempts to locate the referenced method
    // in the superinterfaces of the specified class C:

    // If the maximally-specific superinterface methods of C for the name
    // and descriptor specified by the method reference include exactly one
    // method that does not have its ACC_ABSTRACT flag set, then this method
    // is chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->maximallySpecificInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType, std::not_fn(std::mem_fn(&Method::isAbstract))))
        {
            return method;
        }
    }

    // Otherwise, if any superinterface of C declares a method with the name and descriptor specified by the method
    // reference that has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set, one of these is arbitrarily
    // chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->getAllInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType,
                                     [](const Method& method)
                                     { return !method.isStatic() && method.getVisibility() != Visibility::Private; }))
        {
            return method;
        }
    }

    llvm_unreachable("method not found");
}

const jllvm::Method* LazyClassLoaderHelper::interfaceMethodResolution(const ClassObject* classObject,
                                                                      llvm::StringRef methodName, MethodType methodType,
                                                                      ClassLoader& classLoader)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.4

    // Otherwise, if C declares a method with the name and descriptor specified by the interface method
    // reference, method lookup succeeds.
    if (const Method* method = classObject->getMethod(methodName, methodType))
    {
        return method;
    }

    // Otherwise, if the class Object declares a method with the name and descriptor specified by the
    // interface method reference, which has its ACC_PUBLIC flag set and does not have its ACC_STATIC flag
    // set, method lookup succeeds.
    {
        ClassObject& object = classLoader.forName("Ljava/lang/Object;");
        if (const Method* method =
                object.getMethod(methodName, methodType,
                                 [](const Method& method)
                                 { return !method.isStatic() && method.getVisibility() == Visibility::Public; }))
        {
            return method;
        }
    }

    // Otherwise, if the maximally-specific superinterface methods (§5.4.3.3) of C for the name and
    // descriptor specified by the method reference include exactly one method that does not have its
    // ACC_ABSTRACT flag set, then this method is chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->maximallySpecificInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType, std::not_fn(std::mem_fn(&Method::isAbstract))))
        {
            return method;
        }
    }

    llvm_unreachable("method not found");
}

const jllvm::Method* LazyClassLoaderHelper::specialMethodResolution(const ClassObject* classObject,
                                                                    llvm::StringRef methodName, MethodType methodType,
                                                                    ClassLoader& classLoader,
                                                                    const ClassObject* currentClass,
                                                                    const ClassFile* currentClassFile)
{
    // The named method is resolved (§5.4.3.3, §5.4.3.4).
    const Method* resolvedMethod = classObject->isInterface() ?
                                       interfaceMethodResolution(classObject, methodName, methodType, classLoader) :
                                       methodResolution(classObject, methodName, methodType);
    const ClassObject* resolvedClass = resolvedMethod->getClassObject();

    // If all of the following are true, let C be the direct superclass of the current class:
    //
    // The resolved method is not an instance initialization method (§2.9.1).
    //
    // The symbolic reference names a class (not an interface), and that class is a superclass of the current class.
    //
    // The ACC_SUPER flag is set for the class file (§4.1).
    if (!currentClassFile->hasSuperFlag() || resolvedMethod->isObjectConstructor() || !resolvedClass->isClass()
        || !llvm::is_contained(currentClass->getSuperClasses(/*includeThis=*/false), resolvedClass))
    {
        return resolvedMethod;
    }

    // What follows in the spec is essentially an interface or method resolution but with 'resolvedClass' as the new
    // class.
    resolvedClass = currentClass->getSuperClass();
    return resolvedClass->isInterface() ?
               interfaceMethodResolution(resolvedClass, methodName, methodType, classLoader) :
               methodResolution(resolvedClass, methodName, methodType);
}

llvm::Value* LazyClassLoaderHelper::doStaticCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                 llvm::StringRef methodName, MethodType methodType,
                                                 llvm::ArrayRef<llvm::Value*> args)
{
    return doCallForClassObject(
        builder, className, methodName, methodType, /*isStatic=*/true, "Static Call Stub for", args,
        [=, *this](llvm::IRBuilder<>& builder, const ClassObject* classObject, llvm::ArrayRef<llvm::Value*> args)
        {
            if (!classObject->isInitialized())
            {
                buildClassInitializerInitStub(builder, *classObject);
            }

            const Method* method = classObject->isInterface() ?
                                       interfaceMethodResolution(classObject, methodName, methodType, m_classLoader) :
                                       methodResolution(classObject, methodName, methodType);

            llvm::FunctionType* functionType = descriptorToType(methodType, /*isStatic=*/true, builder.getContext());

            llvm::Module* module = builder.GetInsertBlock()->getModule();
            llvm::CallInst* call =
                builder.CreateCall(module->getOrInsertFunction(mangleDirectMethodCall(method), functionType), args);
            call->setAttributes(getABIAttributes(builder.getContext(), methodType, /*isStatic=*/true));

            return call;
        });
}

llvm::Value* LazyClassLoaderHelper::doInstanceCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                   llvm::StringRef methodName, MethodType methodType,
                                                   llvm::ArrayRef<llvm::Value*> args, MethodResolution resolution)
{
    llvm::StringRef key;
    switch (resolution)
    {
        case Virtual: key = "Virtual Call Stub for"; break;
        case Interface: key = "Interface Call Stub for"; break;
        case Special: key = "Special Call Stub for"; break;
    }
    return doCallForClassObject(
        builder, className, methodName, methodType, false, key, args,
        [=, *this](llvm::IRBuilder<>& builder, const ClassObject* classObject, llvm::ArrayRef<llvm::Value*> args)
        {
            const Method* resolvedMethod;
            switch (resolution)
            {
                case Virtual: resolvedMethod = methodResolution(classObject, methodName, methodType); break;
                case Interface:
                    resolvedMethod = interfaceMethodResolution(classObject, methodName, methodType, m_classLoader);
                    break;
                case Special:
                    resolvedMethod = specialMethodResolution(classObject, methodName, methodType, m_classLoader,
                                                             m_currentClass, m_currentClassFile);
                    break;
            }

            llvm::FunctionType* functionType = descriptorToType(methodType, false, builder.getContext());

            // 'invokespecial' does not do method selection like the others.
            // The spec mentions it as explicitly invoking the resolved method.
            if (resolution == Special || !resolvedMethod->getTableSlot())
            {
                llvm::Module* module = builder.GetInsertBlock()->getModule();
                llvm::CallInst* call = builder.CreateCall(
                    module->getOrInsertFunction(mangleDirectMethodCall(resolvedMethod), functionType), args);
                call->setAttributes(getABIAttributes(builder.getContext(), methodType, /*isStatic=*/false));

                return call;
            }

            if (!resolvedMethod->getClassObject()->isInterface())
            {
                llvm::Value* methodOffset = builder.getInt32(sizeof(VTableSlot) * *resolvedMethod->getTableSlot());
                llvm::Value* thisClassObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
                llvm::Value* vtblPositionInClassObject = builder.getInt32(ClassObject::getVTableOffset());

                llvm::Value* totalOffset = builder.CreateAdd(vtblPositionInClassObject, methodOffset);
                llvm::Value* vtblSlot = builder.CreateGEP(builder.getInt8Ty(), thisClassObject, {totalOffset});
                llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), vtblSlot);

                auto* call = builder.CreateCall(functionType, callee, args);
                call->setAttributes(getABIAttributes(builder.getContext(), methodType, /*isStatic=*/false));
                return call;
            }

            std::size_t sizeTBits = std::numeric_limits<std::size_t>::digits;
            llvm::Value* slot = builder.getIntN(sizeTBits, *resolvedMethod->getTableSlot());
            llvm::Value* id = builder.getIntN(sizeTBits, resolvedMethod->getClassObject()->getInterfaceId());

            llvm::Value* thisClassObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
            llvm::Value* iTablesPtr = builder.CreateGEP(builder.getInt8Ty(), thisClassObject,
                                                        {builder.getInt32(ClassObject::getITablesOffset())});
            llvm::Value* iTables =
                builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(arrayRefType(builder.getContext()), iTablesPtr,
                                                                         {builder.getInt32(0), builder.getInt32(0)}));

            // Linear search over all iTables of 'classObject' until the iTable with the interface id equal to
            // 'id' is found.
            llvm::BasicBlock* pred = builder.GetInsertBlock();
            auto* loopBody = llvm::BasicBlock::Create(builder.getContext(), "", pred->getParent());
            builder.CreateBr(loopBody);

            builder.SetInsertPoint(loopBody);
            llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
            phi->addIncoming(builder.getInt32(0), pred);

            llvm::Value* iTable =
                builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(builder.getPtrTy(), iTables, {phi}));
            llvm::Value* iTableId = builder.CreateLoad(slot->getType(), iTable);
            llvm::Value* cond = builder.CreateICmpEQ(iTableId, id);
            llvm::Value* increment = builder.CreateAdd(phi, builder.getInt32(1));
            phi->addIncoming(increment, loopBody);

            auto* loopContinue = llvm::BasicBlock::Create(builder.getContext(), "", pred->getParent());
            builder.CreateCondBr(cond, loopContinue, loopBody);

            builder.SetInsertPoint(loopContinue);

            llvm::Value* iTableSlot = builder.CreateGEP(iTableType(builder.getContext()), iTable,
                                                        {builder.getInt32(0), builder.getInt32(1), slot});
            llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), iTableSlot);

            auto* call = builder.CreateCall(functionType, callee, args);
            call->setAttributes(getABIAttributes(builder.getContext(), methodType, /*isStatic=*/false));
            return call;
        });
}

llvm::Value* LazyClassLoaderHelper::getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                           llvm::StringRef fieldName, FieldType fieldType)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleFieldAccess(className, fieldName, fieldType),
                                    llvm::FunctionType::get(builder.getIntNTy(sizeof(std::size_t) * 8), false));
    return builder.CreateCall(function);
}

llvm::Value* LazyClassLoaderHelper::getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                          llvm::StringRef fieldName, FieldType fieldType)
{
    llvm::Module* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee function =
        module->getOrInsertFunction(mangleFieldAccess(className, fieldName, fieldType),
                                    llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), false));
    return builder.CreateCall(function);
}

llvm::Value* LazyClassLoaderHelper::getClassObject(llvm::IRBuilder<>& builder, FieldType fieldDescriptor,
                                                   bool mustInitializeClassObject)
{
    return returnConstantForClassObject(
        builder, fieldDescriptor, "", [=](const ClassObject* classObject) { return classObject; },
        mustInitializeClassObject);
}
