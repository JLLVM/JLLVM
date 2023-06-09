#include "CodeGeneratorUtils.hpp"

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
} // namespace

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

    std::string stubSymbol = ("<classLoad>" + fieldDescriptor + key).str();
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
                    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                    if (mustInitializeClassObject && !classObject.isInitialized())
                    {
                        buildClassInitializerInitStub(builder, classObject);
                    }

                    builder.CreateRet(returnValueToIRConstant(builder, f(&classObject)));

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
    std::string stubName = (method + key).str();

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
        if (iter != methods.end())
        {
            if (iter->isFinal())
            {
                // Final method can't be overwritten and we can just create a direct call to it.
                return mangleMethod(curr->getClassName(), iter->getName(), iter->getType());
            }
            return VTableOffset{*iter->getVTableSlot()};
        }
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
        if (method != interface->getMethods().end())
        {
            return ITableOffset{interface->getInterfaceId(), *method->getVTableSlot()};
        }
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
        if (method != interface->getMethods().end())
        {
            return ITableOffset{interface->getInterfaceId(), *method->getVTableSlot()};
        }
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
            return ITableOffset{classObject->getInterfaceId(), *iter->getVTableSlot()};
        }
    }

    // Otherwise, if the class Object declares a method with the name and descriptor specified by the
    // interface method reference, which has its ACC_PUBLIC flag set and does not have its ACC_STATIC flag
    // set, method lookup succeeds.
    {
        constexpr llvm::StringLiteral className = "java/lang/Object";
        llvm::ArrayRef<Method> methods = classLoader.forName("L" + className + ";").getMethods();
        const Method* iter = llvm::find_if(methods,
                                           [&](const Method& method)
                                           {
                                               return !method.isStatic() && method.getVisibility() == Visibility::Public
                                                      && method.getName() == methodName
                                                      && method.getType() == methodType;
                                           });
        if (iter != methods.end())
        {
            return VTableOffset{*iter->getVTableSlot()};
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
        if (method != interface->getMethods().end())
        {
            return ITableOffset{interface->getInterfaceId(), *method->getVTableSlot()};
        }
    }

    llvm_unreachable("method not found");
}

llvm::Value* LazyClassLoaderHelper::doNonVirtualCall(llvm::IRBuilder<>& builder, bool isStatic,
                                                     llvm::StringRef className, llvm::StringRef methodName,
                                                     llvm::StringRef methodType, llvm::ArrayRef<llvm::Value*> args)
{
    return doCallForClassObject(
        builder, className, methodName, methodType, isStatic, "<static>", args,
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
        case Virtual: key = "<virtual>"; break;
        case Interface: key = "<interface>"; break;
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

llvm::Value* LazyClassLoaderHelper::getITableIdAndOffset(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor,
                                                         llvm::StringRef methodName, llvm::StringRef typeDescriptor)
{
    return returnConstantForClassObject(
        builder, fieldDescriptor, methodName + ";" + typeDescriptor,
        [=](const ClassObject* classObject)
        {
            // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.4

            // Otherwise, if C declares a method with the name and descriptor specified by the interface method
            // reference, method lookup succeeds.
            {
                llvm::ArrayRef<Method> methods = classObject->getMethods();
                const Method* iter =
                    llvm::find_if(methods, [&](const Method& method)
                                  { return method.getName() == methodName && method.getType() == typeDescriptor; });
                if (iter != methods.end())
                {
                    return classObject->getInterfaceId() << 8 | *iter->getVTableSlot();
                }
            }

            // TODO:
            // Otherwise, if the class Object declares a method with the name and descriptor specified by the
            // interface method reference, which has its ACC_PUBLIC flag set and does not have its ACC_STATIC flag
            // set, method lookup succeeds.

            // Otherwise, if the maximally-specific superinterface methods (§5.4.3.3) of C for the name and
            // descriptor specified by the method reference include exactly one method that does not have its
            // ACC_ABSTRACT flag set, then this method is chosen and method lookup succeeds.
            for (const ClassObject* interface : classObject->maximallySpecificInterfaces())
            {
                const Method* method = llvm::find_if(interface->getMethods(),
                                                     [&](const Method& method) {
                                                         return !method.isAbstract() && method.getName() == methodName
                                                                && method.getType() == typeDescriptor;
                                                     });
                if (method != interface->getMethods().end())
                {
                    return interface->getInterfaceId() << 8 | *method->getVTableSlot();
                }
            }

            llvm_unreachable("method not found");
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
