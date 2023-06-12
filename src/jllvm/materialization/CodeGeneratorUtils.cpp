#include "CodeGeneratorUtils.hpp"

#include <llvm/IR/DIBuilder.h>

#include "ByteCodeLayer.hpp"
#include "LambdaMaterialization.hpp"

using namespace jllvm;

namespace
{
/// X86 ABI essentially always uses the 32 bit register names for passing along integers. Using the 'signext' and
/// 'zeroext' attribute we tell LLVM that if due to ABI, it has to extend these registers, which extension to use.
/// This attribute list can be applied to either a call or a function itself.
llvm::AttributeList getABIAttributes(llvm::LLVMContext& context, const MethodType& methodType, bool isStatic)
{
    llvm::SmallVector<llvm::AttributeSet> paramAttrs(methodType.parameters.size());
    for (auto&& [param, attrs] : llvm::zip(methodType.parameters, paramAttrs))
    {
        const auto* baseType = get_if<BaseType>(&param);
        if (!baseType || !baseType->isIntegerType())
        {
            continue;
        }
        attrs = attrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }

    llvm::AttributeSet retAttrs;
    if (const auto* baseType = get_if<BaseType>(&methodType.returnType); baseType && baseType->isIntegerType())
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
} // namespace

void ByteCodeTypeChecker::check(const Code& code)
{
    auto* addressType = referenceType(m_context);
    auto* doubleType = llvm::Type::getDoubleTy(m_context);
    auto* floatType = llvm::Type::getFloatTy(m_context);
    auto* intType = llvm::Type::getInt32Ty(m_context);
    auto* longType = llvm::Type::getInt64Ty(m_context);

    for (ByteCodeOp operation : byteCodeRange(code.getCode()))
    {
        match(
            operation,
            [&](AALoad)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
                m_typeStack.push_back(addressType);
            },
            [&](OneOfBase<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
            { m_typeStack.erase(m_typeStack.end() - 3, m_typeStack.end()); },
            [&](OneOfBase<AConstNull, ALoad, ALoad0, ALoad1, ALoad2, ALoad3, New>)
            { m_typeStack.push_back(addressType); },
            [&](OneOfBase<ANewArray, NewArray>)
            {
                m_typeStack.pop_back();
                m_typeStack.push_back(addressType);
            },
            [&](OneOfBase<AReturn, AStore, AStore0, AStore1, AStore2, AStore3, DAdd, DDiv, DMul, DRem, DStore, DStore0,
                          DStore1, DStore2, DStore3, DSub, FAdd, FDiv, FMul, FRem, FReturn, FStore, FStore0, FStore1,
                          FStore2, FStore3, FSub, IAdd, IAnd, IDiv, IfEq, IfNe, IfLt, IfGe, IfGt, IfLe, IfNonNull,
                          IfNull, IMul, IOr, IRem, IReturn, IShl, IShr, IStore, IStore0, IStore1, IStore2, IStore3,
                          ISub, IUShr, IXor, LAdd, LAnd, LDiv, LMul, LookupSwitch, LOr, LRem, LReturn, LShl, LShr,
                          LStore, LStore0, LStore1, LStore2, LStore3, LSub, LUShr, LXor, MonitorEnter, MonitorExit, Pop,
                          PutStatic, TableSwitch>) { m_typeStack.pop_back(); },
            [&](OneOfBase<ArrayLength, D2I, F2I, InstanceOf, L2I>)
            {
                m_typeStack.pop_back();
                m_typeStack.push_back(intType);
            },
            [&](OneOfBase<AThrow, CheckCast, DNeg, FNeg, Goto, GotoW, I2B, I2C, I2S, IInc, INeg, LNeg, Nop, Ret,
                          Return>) { /* Types do not change */ },
            [&](OneOfBase<BALoad, CALoad, DCmpG, DCmpL, FCmpG, FCmpL, IALoad, LCmp, SALoad>)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
                m_typeStack.push_back(intType);
            },
            [&](OneOfBase<BIPush, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4, IConst5, ILoad, ILoad0, ILoad1,
                          ILoad2, ILoad3, SIPush>) { m_typeStack.push_back(intType); },
            [&](OneOfBase<D2F, I2F, L2F>)
            {
                m_typeStack.pop_back();
                m_typeStack.push_back(floatType);
            },
            [&](OneOfBase<D2L, F2L, I2L>)
            {
                m_typeStack.pop_back();
                m_typeStack.push_back(longType);
            },
            [&](DALoad)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
                m_typeStack.push_back(doubleType);
            },
            [&](OneOfBase<DConst0, DConst1, DLoad, DLoad0, DLoad1, DLoad2, DLoad3, DReturn>)
            { m_typeStack.push_back(doubleType); },
            [&](Dup) { m_typeStack.push_back(m_typeStack.back()); },
            [&](DupX1)
            {
                // TODO
            },
            [&](DupX2)
            {
                // TODO
            },
            [&](Dup2)
            {
                // TODO
            },
            [&](Dup2X1)
            {
                // TODO
            },
            [&](Dup2X2)
            {
                // TODO
            },
            [&](OneOfBase<F2D, I2D, L2D>)
            {
                m_typeStack.pop_back();
                m_typeStack.push_back(doubleType);
            },
            [&](FALoad)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
                m_typeStack.push_back(floatType);
            },
            [&](OneOfBase<FConst0, FConst1, FConst2, FLoad, FLoad0, FLoad1, FLoad2, FLoad3>)
            { m_typeStack.push_back(floatType); },
            [&](OneOf<GetField, GetStatic> get)
            {
                FieldType descriptor = parseFieldType(PoolIndex<FieldRefInfo>{get.index}
                                                          .resolve(m_classFile)
                                                          ->nameAndTypeIndex.resolve(m_classFile)
                                                          ->descriptorIndex.resolve(m_classFile)
                                                          ->text);
                llvm::Type* type = descriptorToType(descriptor, m_context);
                if (type->isIntegerTy())
                    ;
            },
            [&](OneOfBase<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, PutField>)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
            },
            [&](InvokeDynamic)
            {
                // TODO
            },
            [&](OneOf<InvokeInterface, InvokeSpecial, InvokeStatic, InvokeVirtual> invoke)
            {
                MethodType descriptor = parseMethodType(PoolIndex<RefInfo>{invoke.index}
                                                            .resolve(m_classFile)
                                                            ->nameAndTypeIndex.resolve(m_classFile)
                                                            ->descriptorIndex.resolve(m_classFile)
                                                            ->text);

                for (auto& _ : descriptor.parameters)
                {
                    m_typeStack.pop_back();
                }

                // static does not pop this
                if (!holds_alternative<InvokeStatic>(operation))
                {
                    m_typeStack.pop_back();
                }
            },
            [&](JSR)
            {
                // TODO
            },
            [&](JSRw)
            {
                // TODO
            },
            [&](LALoad)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
                m_typeStack.push_back(longType);
            },
            [&](OneOfBase<LConst0, LConst1, LLoad, LLoad0, LLoad1, LLoad2, LLoad3>)
            { m_typeStack.push_back(longType); },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(m_classFile), [&](const ClassInfo*) { m_typeStack.push_back(addressType); },
                    [&](const DoubleInfo* doubleInfo) { m_typeStack.push_back(doubleType); },
                    [&](const FloatInfo* floatInfo) { m_typeStack.push_back(floatType); },
                    [&](const IntegerInfo* integerInfo) { m_typeStack.push_back(intType); },
                    [&](const LongInfo* longInfo) { m_typeStack.push_back(longType); },
                    [&](const StringInfo* stringInfo) { m_typeStack.push_back(addressType); },
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
            },
            [&](MultiANewArray multiANewArray)
            {
                for (int i = 0; i < multiANewArray.dimensions; ++i)
                {
                    m_typeStack.pop_back();
                }
            },
            [&](Pop2)
            {
                // TODO
            },
            [&](Swap) { std::swap(m_typeStack.back(), *(m_typeStack.end() - 2)); },
            [&](Wide)
            {
                // TODO
            });
    }
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
llvm::Value* LazyClassLoaderHelper::returnConstantForClassObject(llvm::IRBuilder<>& builder,
                                                                 llvm::Twine fieldDescriptor, llvm::Twine key, F&& f,
                                                                 bool mustInitializeClassObject)
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

    std::string stubSymbol = ("Class load " + fieldDescriptor + key).str();
    if (!m_stubsManager.findStub(stubSymbol, true))
    {
        llvm::cantFail(m_stubsManager.createStub(
            stubSymbol,
            llvm::cantFail(m_callbackManager.getCompileCallback(
                [=, *this, fieldDescriptor = fieldDescriptor.str()]
                {
                    const ClassObject& classObject = m_classLoader.forName(fieldDescriptor);

                    auto context = std::make_unique<llvm::LLVMContext>();
                    auto module = std::make_unique<llvm::Module>(stubSymbol, *context);

                    module->setDataLayout(m_dataLayout);
                    module->setTargetTriple(m_triple.str());

                    auto* functionType = llvm::FunctionType::get(
                        CppToLLVMType<typename llvm::function_traits<std::decay_t<F>>::result_t>::get(context.get()),
                        false);

                    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                                            stubSymbol, module.get());
                    function->addFnAttr(llvm::Attribute::UWTable);
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
                                                         llvm::StringRef methodName, llvm::StringRef methodType,
                                                         bool isStatic, llvm::Twine key,
                                                         llvm::ArrayRef<llvm::Value*> args, F&& f)
{
    MethodType desc = parseMethodType(methodType);
    llvm::FunctionType* functionType = descriptorToType(desc, isStatic, builder.getContext());

    std::string method = mangleMethod(className, methodName, methodType);
    if (const ClassObject* classObject = m_classLoader.forNameLoaded("L" + className + ";"))
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
                [=, *this, desc = std::make_shared<MethodType>(std::move(desc))]
                {
                    const ClassObject& classObject = m_classLoader.forName("L" + className + ";");

                    auto context = std::make_unique<llvm::LLVMContext>();
                    auto module = std::make_unique<llvm::Module>(stubName, *context);

                    module->setDataLayout(m_dataLayout);
                    module->setTargetTriple(m_triple.str());

                    auto* functionType = descriptorToType(*desc, isStatic, *context);

                    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage, stubName,
                                                            module.get());
                    function->addFnAttr(llvm::Attribute::UWTable);
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
    call->setAttributes(getABIAttributes(builder.getContext(), parseMethodType(methodType), isStatic));
    return call;
}

LazyClassLoaderHelper::ResolutionResult LazyClassLoaderHelper::vTableResult(const jllvm::ClassObject* classObject,
                                                                            const jllvm::Method* method)
{
    if (!method->getTableSlot())
    {
        // Methods that can't be overwritten, like final or private methods won't have a v-table slot and we
        // can just create a direct call to them.
        return mangleMethod(classObject->getClassName(), method->getName(), method->getType());
    }
    return VTableOffset{*method->getTableSlot()};
}

LazyClassLoaderHelper::ResolutionResult LazyClassLoaderHelper::iTableResult(const jllvm::ClassObject* interface,
                                                                            const jllvm::Method* method)
{
    if (!method->getTableSlot())
    {
        // Methods that can't be overwritten, like final or private methods won't have a v-table slot and we
        // can just create a direct call to them.
        return mangleMethod(interface->getClassName(), method->getName(), method->getType());
    }
    return ITableOffset{interface->getInterfaceId(), *method->getTableSlot()};
}

LazyClassLoaderHelper::ResolutionResult LazyClassLoaderHelper::virtualMethodResolution(const ClassObject* classObject,
                                                                                       llvm::StringRef methodName,
                                                                                       llvm::StringRef methodType)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.3

    // Otherwise, method resolution attempts to locate the referenced method
    // in C and its superclasses:

    // Otherwise, if C declares a method with the name and descriptor
    // specified by the method reference, method lookup succeeds.

    // Otherwise, if C has a superclass, step 2 of method resolution is
    // recursively invoked on the direct superclass of C.
    for (const ClassObject* curr : classObject->getSuperClasses())
    {
        llvm::ArrayRef<Method> methods = curr->getMethods();
        const Method* iter = llvm::find_if(
            methods, [&](const Method& method)
            { return !method.isStatic() && method.getName() == methodName && method.getType() == methodType; });
        if (iter == methods.end())
        {
            continue;
        }
        return vTableResult(curr, iter);
    }

    // Otherwise, method resolution attempts to locate the referenced method
    // in the superinterfaces of the specified class C:

    // If the maximally-specific superinterface methods of C for the name
    // and descriptor specified by the method reference include exactly one
    // method that does not have its ACC_ABSTRACT flag set, then this method
    // is chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->maximallySpecificInterfaces())
    {
        const Method* method = llvm::find_if(
            interface->getMethods(), [&](const Method& method)
            { return !method.isAbstract() && method.getName() == methodName && method.getType() == methodType; });
        if (method == interface->getMethods().end())
        {
            continue;
        }
        return iTableResult(interface, method);
    }

    // Otherwise, if any superinterface of C declares a method with the name and descriptor specified by the method
    // reference that has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set, one of these is arbitrarily
    // chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->getAllInterfaces())
    {
        const Method* method =
            llvm::find_if(interface->getMethods(),
                          [&](const Method& method)
                          {
                              return !method.isStatic() && method.getVisibility() != Visibility::Private
                                     && method.getName() == methodName && method.getType() == methodType;
                          });
        if (method == interface->getMethods().end())
        {
            continue;
        }
        return iTableResult(interface, method);
    }

    llvm_unreachable("method not found");
}
LazyClassLoaderHelper::ResolutionResult LazyClassLoaderHelper::interfaceMethodResolution(const ClassObject* classObject,
                                                                                         llvm::StringRef methodName,
                                                                                         llvm::StringRef methodType,
                                                                                         ClassLoader& classLoader)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.4

    // Otherwise, if C declares a method with the name and descriptor specified by the interface method
    // reference, method lookup succeeds.
    {
        llvm::ArrayRef<Method> methods = classObject->getMethods();
        const Method* iter =
            llvm::find_if(methods, [&](const Method& method)
                          { return method.getName() == methodName && method.getType() == methodType; });
        if (iter != methods.end())
        {
            return iTableResult(classObject, iter);
        }
    }

    // Otherwise, if the class Object declares a method with the name and descriptor specified by the
    // interface method reference, which has its ACC_PUBLIC flag set and does not have its ACC_STATIC flag
    // set, method lookup succeeds.
    {
        constexpr llvm::StringLiteral className = "java/lang/Object";
        ClassObject& object = classLoader.forName("L" + className + ";");
        llvm::ArrayRef<Method> methods = object.getMethods();
        const Method* iter = llvm::find_if(methods,
                                           [&](const Method& method)
                                           {
                                               return !method.isStatic() && method.getVisibility() == Visibility::Public
                                                      && method.getName() == methodName
                                                      && method.getType() == methodType;
                                           });
        if (iter != methods.end())
        {
            return vTableResult(&object, iter);
        }
    }

    // Otherwise, if the maximally-specific superinterface methods (§5.4.3.3) of C for the name and
    // descriptor specified by the method reference include exactly one method that does not have its
    // ACC_ABSTRACT flag set, then this method is chosen and method lookup succeeds.
    for (const ClassObject* interface : classObject->maximallySpecificInterfaces())
    {
        const Method* method = llvm::find_if(
            interface->getMethods(), [&](const Method& method)
            { return !method.isAbstract() && method.getName() == methodName && method.getType() == methodType; });
        if (method == interface->getMethods().end())
        {
            continue;
        }
        return iTableResult(interface, method);
    }

    llvm_unreachable("method not found");
}

llvm::Value* LazyClassLoaderHelper::doNonVirtualCall(llvm::IRBuilder<>& builder, bool isStatic,
                                                     llvm::StringRef className, llvm::StringRef methodName,
                                                     llvm::StringRef methodType, llvm::ArrayRef<llvm::Value*> args)
{
    return doCallForClassObject(
        builder, className, methodName, methodType, isStatic, "Static Call Stub for", args,
        [isStatic, className = className.str(), methodName = methodName.str(), methodType = methodType.str()](
            llvm::IRBuilder<>& builder, const ClassObject* classObject, llvm::ArrayRef<llvm::Value*> args)
        {
            if (isStatic && !classObject->isInitialized())
            {
                buildClassInitializerInitStub(builder, *classObject);
            }

            MethodType desc = parseMethodType(methodType);
            llvm::FunctionType* functionType = descriptorToType(desc, isStatic, builder.getContext());

            std::string method = mangleMethod(className, methodName, methodType);

            llvm::Module* module = builder.GetInsertBlock()->getModule();
            llvm::CallInst* call = builder.CreateCall(module->getOrInsertFunction(method, functionType), args);
            call->setAttributes(getABIAttributes(builder.getContext(), parseMethodType(methodType), isStatic));

            return call;
        });
}

llvm::Value* LazyClassLoaderHelper::doIndirectCall(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                   llvm::StringRef methodName, llvm::StringRef methodType,
                                                   llvm::ArrayRef<llvm::Value*> args, MethodResolution resolution)
{
    llvm::StringRef key;
    switch (resolution)
    {
        case Virtual: key = "Virtual Call Stub for"; break;
        case Interface: key = "Interface Call Stub for"; break;
    }
    return doCallForClassObject(
        builder, className, methodName, methodType, false, key, args,
        [className = className.str(), methodName = methodName.str(), methodType = methodType.str(), resolution,
         &classLoader = m_classLoader](llvm::IRBuilder<>& builder, const ClassObject* classObject,
                                       llvm::ArrayRef<llvm::Value*> args)
        {
            ResolutionResult resolutionResult;
            switch (resolution)
            {
                case Virtual: resolutionResult = virtualMethodResolution(classObject, methodName, methodType); break;
                case Interface:
                    resolutionResult = interfaceMethodResolution(classObject, methodName, methodType, classLoader);
                    break;
            }

            MethodType desc = parseMethodType(methodType);
            llvm::FunctionType* functionType = descriptorToType(desc, false, builder.getContext());

            if (auto* directCallee = get_if<std::string>(&resolutionResult))
            {
                llvm::Module* module = builder.GetInsertBlock()->getModule();
                llvm::CallInst* call =
                    builder.CreateCall(module->getOrInsertFunction(*directCallee, functionType), args);
                call->setAttributes(getABIAttributes(builder.getContext(), desc, /*isStatic=*/false));

                return call;
            }

            if (auto* virtualCallee = get_if<VTableOffset>(&resolutionResult))
            {
                llvm::Value* methodOffset = builder.getInt32(sizeof(VTableSlot) * virtualCallee->slot);
                llvm::Value* thisClassObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
                llvm::Value* vtblPositionInClassObject = builder.getInt32(ClassObject::getVTableOffset());

                llvm::Value* totalOffset = builder.CreateAdd(vtblPositionInClassObject, methodOffset);
                llvm::Value* vtblSlot = builder.CreateGEP(builder.getInt8Ty(), thisClassObject, {totalOffset});
                llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), vtblSlot);

                auto* call = builder.CreateCall(functionType, callee, args);
                call->setAttributes(getABIAttributes(builder.getContext(), desc, /*isStatic=*/false));
                return call;
            }

            auto& iTableOffset = get<ITableOffset>(resolutionResult);
            std::size_t sizeTBits = std::numeric_limits<std::size_t>::digits;
            llvm::Value* slot = builder.getIntN(sizeTBits, iTableOffset.slot);
            llvm::Value* id = builder.getIntN(sizeTBits, iTableOffset.interfaceId);

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
            call->setAttributes(getABIAttributes(builder.getContext(), desc, /*isStatic=*/false));
            return call;
        });
}

llvm::Value* LazyClassLoaderHelper::getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                           llvm::StringRef fieldName, llvm::StringRef fieldType)
{
    return returnConstantForClassObject(
        builder, "L" + className + ";", fieldName + ";" + fieldType,
        [=](const ClassObject* classObject)
        {
            return classObject
                ->getField(fieldName, fieldType,
                           /*isStatic=*/false)
                ->getOffset();
        },
        /*mustInitializeClassObject=*/false);
}

llvm::Value* LazyClassLoaderHelper::getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                                          llvm::StringRef fieldName, llvm::StringRef fieldType)
{
    return returnConstantForClassObject(
        builder, "L" + className + ";", fieldName + ";" + fieldType,
        [=](const ClassObject* classObject)
        { return classObject->getField(fieldName, fieldType, /*isStatic=*/true)->getAddressOfStatic(); },
        /*mustInitializeClassObject=*/true);
}

llvm::Value* LazyClassLoaderHelper::getClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor,
                                                   bool mustInitializeClassObject)
{
    return returnConstantForClassObject(
        builder, fieldDescriptor, "", [=](const ClassObject* classObject) { return classObject; },
        mustInitializeClassObject);
}
