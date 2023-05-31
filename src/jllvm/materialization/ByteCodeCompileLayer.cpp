#include "ByteCodeCompileLayer.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#include <jllvm/class/ByteCodeIterator.hpp>
#include <jllvm/class/Descriptors.hpp>
#include <jllvm/object/Object.hpp>

#include <ranges>
#include <utility>

#include "ByteCodeCompileUtils.hpp"
#include "LambdaMaterialization.hpp"

#define DEBUG_TYPE "jvm"

using namespace jllvm;

template <>
struct jllvm::CppToLLVMType<const jllvm::ClassObject*>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return referenceType(*context);
    }

    static llvm::Value* getConstant(const jllvm::ClassObject* classObject, llvm::IRBuilder<>& builder)
    {
        return builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uintptr_t>(classObject)),
                                      get(&builder.getContext()));
    }
};

template <>
struct jllvm::CppToLLVMType<jllvm::ClassObject*> : CppToLLVMType<const jllvm::ClassObject*>
{
};

namespace
{
auto objectHeaderType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(/*classObject*/ referenceType(context), /*hashCode*/ llvm::Type::getInt32Ty(context));
}

auto arrayStructType(llvm::Type* elementType)
{
    return llvm::StructType::get(elementType->getContext(), {objectHeaderType(elementType->getContext()),
                                                             llvm::Type::getInt32Ty(elementType->getContext()),
                                                             llvm::ArrayType::get(elementType, 0)});
}

auto arrayRefType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(context, {llvm::PointerType::get(context, 0),
                                           llvm::Type::getIntNTy(context, std::numeric_limits<std::size_t>::digits)});
}

auto iTableType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(context, {llvm::Type::getIntNTy(context, std::numeric_limits<std::size_t>::digits),
                                           llvm::ArrayType::get(llvm::PointerType::get(context, 0), 0)});
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

llvm::GlobalVariable* activeException(llvm::Module* module)
{
    return llvm::cast<llvm::GlobalVariable>(
        module->getOrInsertGlobal("activeException", referenceType(module->getContext())));
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

/// X86 ABI essentially always uses the 32 bit register names for passing along integers. Using the 'signext' and
/// 'zeroext' attribute we tell LLVM that if due to ABI, it has to extend these registers, which extension to use.
/// This attribute list can be applied to either a call or a function itself.
llvm::AttributeList getABIAttributes(llvm::LLVMContext& context, const jllvm::MethodType& methodType, bool isStatic)
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

/// Class for operand stack
/// This class also offers method to get and set the current top of stack in order to consider the control flow path
class OperandStack
{
    std::vector<llvm::AllocaInst*> m_values;
    llvm::AllocaInst** m_topOfStack;
    llvm::IRBuilder<>& m_builder;

public:
    OperandStack(u_int16_t maxStack, llvm::IRBuilder<>& builder)
        : m_builder(builder), m_values(maxStack), m_topOfStack(m_values.data())
    {
        for (auto& alloca : m_values)
        {
            alloca = builder.CreateAlloca(llvm::PointerType::get(builder.getContext(), 0));
        }
    }

    llvm::Value* pop_back(llvm::Type* ty)
    {
        return m_builder.CreateLoad(ty, *(--m_topOfStack));
    }

    void push_back(llvm::Value* value)
    {
        m_builder.CreateStore(value, *(m_topOfStack++));
    }

    llvm::AllocaInst** getTopOfStack() const
    {
        return m_topOfStack;
    }

    llvm::AllocaInst* getBottomOfStack() const
    {
        return m_values.front();
    }

    void setTopOfStack(llvm::AllocaInst** topOfStack)
    {
        m_topOfStack = topOfStack;
    }
};

/// Helper class to fetch properties about a class while still doing lazy class loading.
/// This works by taking callbacks which are either called immediately if a class object is already loaded, leading
/// to better code generation, or otherwise creating stubs that when called load the given class object and return
/// the value given by the callback.
class LazyClassLoaderHelper
{
    ClassLoader& m_classLoader;
    llvm::orc::JITDylib& m_mainDylib;
    llvm::orc::JITDylib& m_implDylib;
    llvm::orc::IndirectStubsManager& m_stubsManager;
    llvm::orc::JITCompileCallbackManager& m_callbackManager;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::orc::MangleAndInterner& m_interner;
    llvm::DataLayout m_dataLayout;
    llvm::Triple m_triple;

    template <class F>
    llvm::Value* returnConstantForClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor, llvm::Twine key,
                                              F&& f)
    {
        auto returnValueToIRConstant = [](llvm::IRBuilder<>& builder, const auto& retVal)
        {
            using RetType = std::decay_t<decltype(retVal)>;
            return CppToLLVMType<RetType>::getConstant(retVal, builder);
        };

        if (const ClassObject* classObject = m_classLoader.forNameLoaded(fieldDescriptor))
        {
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
                            CppToLLVMType<typename llvm::function_traits<std::decay_t<F>>::result_t>::get(
                                context.get()),
                            false);

                        auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                                                stubSymbol, module.get());
                        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                        builder.CreateRet(returnValueToIRConstant(builder, f(&classObject)));

                        llvm::cantFail(m_baseLayer.add(
                            m_implDylib, llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

                        auto address = llvm::cantFail(m_implDylib.getExecutionSession().lookup({&m_implDylib},
                                                                                               m_interner(stubSymbol)))
                                           .getAddress();

                        llvm::cantFail(m_stubsManager.updatePointer(stubSymbol, address));

                        return address;
                    })),
                llvm::JITSymbolFlags::Exported));
            llvm::cantFail(m_mainDylib.define(
                llvm::orc::absoluteSymbols({{m_interner(stubSymbol), m_stubsManager.findStub(stubSymbol, true)}})));
        }

        auto* functionType = llvm::FunctionType::get(
            CppToLLVMType<typename llvm::function_traits<std::decay_t<F>>::result_t>::get(&builder.getContext()),
            false);

        llvm::Module* module = builder.GetInsertBlock()->getModule();
        llvm::FunctionCallee function = module->getOrInsertFunction(stubSymbol, functionType);
        return builder.CreateCall(function);
    }

public:
    LazyClassLoaderHelper(ClassLoader& classLoader, llvm::orc::JITDylib& mainDylib, llvm::orc::JITDylib& implDylib,
                          llvm::orc::IndirectStubsManager& stubsManager,
                          llvm::orc::JITCompileCallbackManager& callbackManager, llvm::orc::IRLayer& baseLayer,
                          llvm::orc::MangleAndInterner& interner, const llvm::DataLayout& dataLayout)
        : m_mainDylib(mainDylib),
          m_implDylib(implDylib),
          m_stubsManager(stubsManager),
          m_callbackManager(callbackManager),
          m_baseLayer(baseLayer),
          m_dataLayout(dataLayout),
          m_classLoader(classLoader),
          m_interner(interner)
    {
    }

    /// Returns a pointer to the function 'methodName' of the type 'methodType' within 'className.
    llvm::Value* getNonVirtualCallee(llvm::IRBuilder<>& builder, bool isStatic, llvm::StringRef className,
                                     llvm::StringRef methodName, llvm::StringRef methodType)
    {
        MethodType desc = parseMethodType(methodType);
        llvm::FunctionType* functionType = descriptorToType(desc, isStatic, builder.getContext());

        std::string method = mangleMethod(className, methodName, methodType);
        if (m_classLoader.forNameLoaded("L" + className + ";"))
        {
            // If the class loader is present then the function should have already been registered and we can just
            // return it directly.
            llvm::Module* module = builder.GetInsertBlock()->getModule();
            return module->getOrInsertFunction(method, functionType).getCallee();
        }

        // Otherwise we create a stub to call the class loader at runtime and then later replace the stub with the
        // real method.
        std::string stubName = method + "<stub>";

        llvm::Module* module = builder.GetInsertBlock()->getModule();
        llvm::Value* result = module->getOrInsertFunction(stubName, functionType).getCallee();

        if (!m_stubsManager.findStub(stubName, true))
        {
            // Create the stub if it hasn't yet been created.
            llvm::cantFail(m_stubsManager.createStub(
                stubName,
                llvm::cantFail(m_callbackManager.getCompileCallback(
                    [=, *this]
                    {
                        m_classLoader.forName("L" + className + ";");
                        auto address =
                            llvm::cantFail(m_mainDylib.getExecutionSession().lookup({&m_mainDylib}, m_interner(method)))
                                .getAddress();
                        llvm::cantFail(m_stubsManager.updatePointer(stubName, address));
                        return address;
                    })),
                llvm::JITSymbolFlags::Exported));

            llvm::cantFail(m_mainDylib.define(
                llvm::orc::absoluteSymbols({{m_interner(stubName), m_stubsManager.findStub(stubName, true)}})));
        }

        return result;
    }

    /// Returns an LLVM integer constant which contains the offset of the 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                        llvm::StringRef fieldName, llvm::StringRef fieldType)
    {
        return returnConstantForClassObject(builder, "L" + className + ";", fieldName + ";" + fieldType,
                                            [=](const ClassObject* classObject)
                                            {
                                                return classObject
                                                    ->getField(fieldName, fieldType,
                                                               /*isStatic=*/false)
                                                    ->getOffset();
                                            });
    }

    llvm::Value* getVTableOffset(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor, llvm::StringRef methodName,
                                 llvm::StringRef typeDescriptor)
    {
        return returnConstantForClassObject(builder, fieldDescriptor, methodName + ";" + typeDescriptor,
                                            [=](const ClassObject* classObject)
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
                                                    const Method* iter =
                                                        llvm::find_if(methods,
                                                                      [&](const Method& method) {
                                                                          return !method.isStatic()
                                                                                 && method.getName() == methodName
                                                                                 && method.getType() == typeDescriptor;
                                                                      });
                                                    if (iter != methods.end())
                                                    {
                                                        return *iter->getVTableSlot();
                                                    }
                                                }

                                                // TODO: Implement below. Requires a vtable slot per implementing class
                                                //       For any default interface method.

                                                // Otherwise, method resolution attempts to locate the referenced method
                                                // in the superinterfaces of the specified class C:

                                                // If the maximally-specific superinterface methods of C for the name
                                                // and descriptor specified by the method reference include exactly one
                                                // method that does not have its ACC_ABSTRACT flag set, then this method
                                                // is chosen and method lookup succeeds.

                                                llvm_unreachable("method not found");
                                            });
    }

    /// Returns an LLVM integer containing the iTable offset in the lower 8 bits and the id of the interface, whose
    /// iTable should be indexed into from the 9th bit onwards for the class indicated by 'fieldDescriptor',
    /// the method named 'methodName' with the type 'typeDescriptor'.
    llvm::Value* getITableIdAndOffset(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor,
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
                for (const jllvm::ClassObject* interface : classObject->maximallySpecificInterfaces())
                {
                    const Method* method = llvm::find_if(interface->getMethods(),
                                                         [&](const jllvm::Method& method) {
                                                             return !method.isAbstract()
                                                                    && method.getName() == methodName
                                                                    && method.getType() == typeDescriptor;
                                                         });
                    if (method != interface->getMethods().end())
                    {
                        return interface->getInterfaceId() << 8 | *method->getVTableSlot();
                    }
                }

                llvm_unreachable("method not found");
            });
    }

    /// Returns an LLVM Pointer which points to the static field 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef fieldName,
                                       llvm::StringRef fieldType)
    {
        return returnConstantForClassObject(
            builder, "L" + className + ";", fieldName + ";" + fieldType,
            [=](const ClassObject* classObject)
            { return classObject->getField(fieldName, fieldType, /*isStatic=*/true)->getAddressOfStatic(); });
    }

    /// Returns an LLVM Pointer which points to the class object of the type with the given field descriptor.
    llvm::Value* getClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor)
    {
        return returnConstantForClassObject(builder, fieldDescriptor, "",
                                            [=](const ClassObject* classObject) { return classObject; });
    }
};

llvm::Type* ensureI32(llvm::Type* llvmFieldType, llvm::IRBuilder<>& builder)
{
    return !llvmFieldType->isIntegerTy() || llvmFieldType->getIntegerBitWidth() >= 32 ? llvmFieldType :
                                                                                        builder.getInt32Ty();
}

auto resolveNewArrayInfo(ArrayOp::ArrayType arrayType, llvm::IRBuilder<>& builder)
{
    struct ArrayInfo
    {
        llvm::StringRef descriptor;
        llvm::Type* type{};
        std::size_t size{};
        std::size_t elementOffset{};
    };

    switch (arrayType)
    {
        case ArrayOp::ArrayType::TBoolean:
            return ArrayInfo{"Z", builder.getInt8Ty(), sizeof(std::uint8_t),
                             jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TChar:
            return ArrayInfo{"C", builder.getInt16Ty(), sizeof(std::uint16_t),
                             jllvm::Array<std::uint16_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TFloat:
            return ArrayInfo{"F", builder.getFloatTy(), sizeof(float), jllvm::Array<float>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TDouble:
            return ArrayInfo{"D", builder.getDoubleTy(), sizeof(double), jllvm::Array<double>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TByte:
            return ArrayInfo{"B", builder.getInt8Ty(), sizeof(std::uint8_t),
                             jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TShort:
            return ArrayInfo{"S", builder.getInt16Ty(), sizeof(std::int16_t),
                             jllvm::Array<std::int16_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TInt:
            return ArrayInfo{"I", builder.getInt32Ty(), sizeof(std::int32_t),
                             jllvm::Array<std::int32_t>::arrayElementsOffset()};
        case ArrayOp::ArrayType::TLong:
            return ArrayInfo{"J", builder.getInt64Ty(), sizeof(std::int64_t),
                             jllvm::Array<std::int64_t>::arrayElementsOffset()};
        default: llvm_unreachable("Invalid array type");
    }
}

struct CodeGen
{
    llvm::Function* function;
    const ClassFile& classFile;
    LazyClassLoaderHelper helper;
    StringInterner& stringInterner;
    const MethodType& functionMethodType;
    llvm::IRBuilder<> builder;
    OperandStack operandStack;
    std::vector<llvm::AllocaInst*> locals;
    llvm::DenseMap<std::uint16_t, llvm::BasicBlock*> basicBlocks;
    llvm::DenseMap<llvm::BasicBlock*, llvm::AllocaInst**> basicBlockStackPointers;

    using HandlerInfo = std::pair<std::uint16_t, PoolIndex<ClassInfo>>;

    // std::list because we want the iterator stability when deleting handlers (requires random access).
    std::list<HandlerInfo> activeHandlers;
    // std::map because it is the easiest to use with std::list key.
    std::map<std::list<HandlerInfo>, llvm::BasicBlock*> alreadyGeneratedEHHandlers;

    CodeGen(llvm::Function* function, const Code& code, const ClassFile& classFile, LazyClassLoaderHelper helper,
            StringInterner& stringInterner, const MethodType& methodType)
        : function(function),
          classFile(classFile),
          helper(std::move(helper)),
          stringInterner(stringInterner),
          functionMethodType(methodType),
          builder(llvm::BasicBlock::Create(function->getContext(), "entry", function)),
          operandStack(code.getMaxStack(), builder),
          locals(code.getMaxLocals())
    {
        for (auto& alloca : locals)
        {
            // We need pointer size bytes, since that is the largest type we may store in a local.
            alloca = builder.CreateAlloca(llvm::PointerType::get(function->getContext(), 0));
        }

        // Arguments are put into the locals. According to the specification, i64s and doubles are split into two
        // locals. We don't actually do that, we just put them into the very first local, but we still have to skip over
        // the following local as if we didn't.
        auto nextLocal = locals.begin();
        for (auto& arg : function->args())
        {
            builder.CreateStore(&arg, *nextLocal++);
            if (arg.getType()->isIntegerTy(64) || arg.getType()->isDoubleTy())
            {
                nextLocal++;
            }
        }

        calculateBasicBlocks(code);
        codeGenBody(code);
    }

    void calculateBasicBlocks(const Code& code)
    {
        for (ByteCodeOp operation : byteCodeRange(code.getCode()))
        {
            auto addBasicBlock = [&](std::uint16_t target)
            {
                auto [result, inserted] = basicBlocks.insert({target, nullptr});

                if (inserted)
                {
                    result->second = llvm::BasicBlock::Create(builder.getContext(), "", function);
                }
            };
            match(
                operation, [&](OneOf<Goto, GotoW> gotoOp) { addBasicBlock(gotoOp.target + gotoOp.offset); },
                [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe,
                          IfLt, IfGe, IfGt, IfLe, IfNonNull, IfNull>
                        cmpOp)
                {
                    addBasicBlock(cmpOp.target + cmpOp.offset);
                    addBasicBlock(cmpOp.offset + sizeof(OpCodes) + sizeof(int16_t));
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
            // required that we register that fact in 'basicBlockStackPointers' explicitly.
            result->second = llvm::BasicBlock::Create(builder.getContext(), "", function);
            basicBlockStackPointers.insert({result->second, std::next(operandStack.getTopOfStack())});
        }
    }

    llvm::BasicBlock* generateEHHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred)
    {
        llvm::IRBuilder<>::InsertPointGuard guard{builder};

        auto result = alreadyGeneratedEHHandlers.find(activeHandlers);
        if (result != alreadyGeneratedEHHandlers.end())
        {
            llvm::BasicBlock* block = result->second;
            // Adding new predecessors exception object to phi node.
            llvm::cast<llvm::PHINode>(&block->front())->addIncoming(exception, newPred);
            return block;
        }

        auto* ehHandler = llvm::BasicBlock::Create(builder.getContext(), "", function);
        alreadyGeneratedEHHandlers.emplace(activeHandlers, ehHandler);
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
                builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(function->getParent()));
                builder.CreateStore(phi, operandStack.getBottomOfStack());
                builder.CreateBr(handlerBB);
                return ehHandler;
            }

            // Since an exception class must be loaded for any instance of the class to be created, we can be
            // certain that the exception is not of the type if the class has not yet been loaded. And most
            // importantly, don't need to eagerly load it.
            llvm::FunctionCallee forNameLoaded =
                function->getParent()->getOrInsertFunction("jllvm_for_name_loaded", ty, builder.getPtrTy());
            llvm::SmallString<64> buffer;
            llvm::Value* className = builder.CreateGlobalStringPtr(
                ("L" + catchType.resolve(classFile)->nameIndex.resolve(classFile)->text + ";").toStringRef(buffer));
            llvm::Value* classObject = builder.CreateCall(forNameLoaded, className);
            llvm::Value* notLoaded = builder.CreateICmpEQ(classObject, llvm::ConstantPointerNull::get(ty));

            auto* nextHandler = llvm::BasicBlock::Create(builder.getContext(), "", function);
            auto* instanceOfCheck = llvm::BasicBlock::Create(builder.getContext(), "", function);
            builder.CreateCondBr(notLoaded, nextHandler, instanceOfCheck);

            builder.SetInsertPoint(instanceOfCheck);

            llvm::FunctionCallee callee = function->getParent()->getOrInsertFunction(
                "jllvm_instance_of", builder.getInt32Ty(), ty, classObject->getType());
            llvm::Value* call = builder.CreateCall(callee, {phi, classObject});
            call = builder.CreateTrunc(call, builder.getInt1Ty());

            auto* jumpToHandler = llvm::BasicBlock::Create(builder.getContext(), "", function);
            builder.CreateCondBr(call, jumpToHandler, nextHandler);

            builder.SetInsertPoint(jumpToHandler);
            // Set exception object as only object on the stack and clear the active exception.
            builder.CreateStore(phi, operandStack.getBottomOfStack());
            builder.CreateStore(llvm::ConstantPointerNull::get(ty), activeException(function->getParent()));
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

    void generateEHDispatch()
    {
        llvm::PointerType* referenceTy = referenceType(builder.getContext());
        llvm::Value* value = builder.CreateLoad(referenceTy, activeException(function->getParent()));
        llvm::Value* cond = builder.CreateICmpEQ(value, llvm::ConstantPointerNull::get(referenceTy));

        auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", function);
        builder.CreateCondBr(cond, continueBlock, generateEHHandlerChain(value, builder.GetInsertBlock()));

        builder.SetInsertPoint(continueBlock);
    }

    llvm::Value* generateAllocArray(llvm::StringRef descriptor, llvm::Value* classObject, llvm::Value* size)
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
        llvm::Value* array = builder.CreateCall(allocationFunction(function->getParent()), bytesNeeded);

        builder.CreateStore(classObject, array);

        llvm::Value* gep =
            builder.CreateGEP(arrayStructType(elementType), array, {builder.getInt32(0), builder.getInt32(1)});
        builder.CreateStore(size, gep);

        return array;
    }

    void codeGenBody(const Code& code);

    void codeGenInstruction(ByteCodeOp operation);
};

void CodeGen::codeGenBody(const Code& code)
{
    llvm::DenseMap<std::uint16_t, std::vector<Code::ExceptionTable>> startHandlers;
    for (const auto& iter : code.getExceptionTable())
    {
        startHandlers[iter.startPc].push_back(iter);
    }

    llvm::DenseMap<std::uint16_t, std::vector<std::list<HandlerInfo>::iterator>> endHandlers;
    for (ByteCodeOp operation : byteCodeRange(code.getCode()))
    {
        if (auto result = endHandlers.find(getOffset(operation)); result != endHandlers.end())
        {
            for (auto iter : result->second)
            {
                activeHandlers.erase(iter);
            }
            // No longer needed.
            endHandlers.erase(result);
        }

        if (auto result = startHandlers.find(getOffset(operation)); result != startHandlers.end())
        {
            for (const Code::ExceptionTable& iter : result->second)
            {
                activeHandlers.emplace_back(iter.handlerPc, iter.catchType);
                endHandlers[iter.endPc].push_back(std::prev(activeHandlers.end()));
            }
            // No longer needed.
            startHandlers.erase(result);
        }

        if (auto result = basicBlocks.find(getOffset(operation)); result != basicBlocks.end())
        {
            // Without any branches, there will not be a terminator at the end of the basic block. Thus, we need to
            // set this manually to the new insert point. This essentially implements implicit fallthrough from JVM
            // bytecode.
            if (builder.GetInsertBlock()->getTerminator() == nullptr)
            {
                basicBlockStackPointers.insert({result->second, operandStack.getTopOfStack()});
                builder.CreateBr(result->second);
            }
            builder.SetInsertPoint(result->second);
            if (auto resultStackPointer = basicBlockStackPointers.find(result->second);
                resultStackPointer != basicBlockStackPointers.end())
            {
                operandStack.setTopOfStack(resultStackPointer->second);
            }
        }

        codeGenInstruction(std::move(operation));
    }
}

void CodeGen::codeGenInstruction(ByteCodeOp operation)
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

            llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

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
            auto [popType, arrayType] = match(
                operation,
                [](...) -> std::pair<llvm::Type*, llvm::Type*> { llvm_unreachable("Invalid array load operation"); },
                [&](AAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {referenceType(builder.getContext()), referenceType(builder.getContext())};
                },
                [&](BAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getInt32Ty(), builder.getInt8Ty()};
                },
                [&](OneOf<CAStore, SAStore>) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getInt32Ty(), builder.getInt16Ty()};
                },
                [&](DAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getDoubleTy(), builder.getDoubleTy()};
                },
                [&](FAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getFloatTy(), builder.getFloatTy()};
                },
                [&](IAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getInt32Ty(), builder.getInt32Ty()};
                },
                [&](LAStore) -> std::pair<llvm::Type*, llvm::Type*> {
                    return {builder.getInt64Ty(), builder.getInt64Ty()};
                });

            llvm::Value* value = operandStack.pop_back(popType);
            llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
            // TODO: throw NullPointerException if array is null
            llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

            // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
            llvm::Value* gep =
                builder.CreateGEP(arrayStructType(arrayType), array, {builder.getInt32(0), builder.getInt32(2), index});
            match(
                operation, [](...) {},
                [&, arrayType = arrayType](OneOf<BAStore, CAStore, SAStore>)
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
            llvm::Value* count = operandStack.pop_back(builder.getInt32Ty());

            llvm::Value* classObject = helper.getClassObject(
                builder, "[L" + index.resolve(classFile)->nameIndex.resolve(classFile)->text + ";");
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(classObject))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            // Size required is the size of the array prior to the elements (equal to the offset to the
            // elements) plus element count * element size.
            llvm::Value* bytesNeeded = builder.getInt32(Array<>::arrayElementsOffset());
            bytesNeeded = builder.CreateAdd(bytesNeeded, builder.CreateMul(count, builder.getInt32(sizeof(Object*))));

            llvm::Value* object = builder.CreateCall(allocationFunction(function->getParent()), bytesNeeded);
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
            llvm::Type* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid load operation"); },
                [&](AReturn) { return referenceType(builder.getContext()); },
                [&](DReturn) { return builder.getDoubleTy(); }, [&](FReturn) { return builder.getFloatTy(); },
                [&](IReturn) { return builder.getInt32Ty(); }, [&](LReturn) { return builder.getInt64Ty(); });

            llvm::Value* value = operandStack.pop_back(type);

            match(
                operation, [](...) {},
                [&](IReturn)
                {
                    if (functionMethodType.returnType == FieldType(BaseType::Boolean))
                    {
                        value = builder.CreateAnd(value, builder.getInt32(1));
                    }
                    if (function->getReturnType() != value->getType())
                    {
                        value = builder.CreateTrunc(value, function->getReturnType());
                    }
                });

            builder.CreateRet(value);
        },
        [&](ArrayLength)
        {
            llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

            // The element type of the array type here is actually irrelevant.
            llvm::Value* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                                 {builder.getInt32(0), builder.getInt32(1)});
            operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), gep));
        },
        [&](OneOf<AStore, DStore, FStore, IStore, LStore> store)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid store operation"); },
                [&](AStore) { return referenceType(builder.getContext()); },
                [&](DStore) { return builder.getDoubleTy(); }, [&](FStore) { return builder.getFloatTy(); },
                [&](IStore) { return builder.getInt32Ty(); }, [&](LStore) { return builder.getInt64Ty(); });

            builder.CreateStore(operandStack.pop_back(type), locals[store.index]);
        },
        [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0, AStore1, DStore1, FStore1, IStore1, LStore1, AStore2,
                  DStore2, FStore2, IStore2, LStore2, AStore3, DStore3, FStore3, IStore3, LStore3>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid store operation"); },
                [&](OneOf<AStore0, AStore1, AStore2, AStore3>) { return referenceType(builder.getContext()); },
                [&](OneOf<DStore0, DStore1, DStore2, DStore3>) { return builder.getDoubleTy(); },
                [&](OneOf<FStore0, FStore1, FStore2, FStore3>) { return builder.getFloatTy(); },
                [&](OneOf<IStore0, IStore1, IStore2, IStore3>) { return builder.getInt32Ty(); },
                [&](OneOf<LStore0, LStore1, LStore2, LStore3>) { return builder.getInt64Ty(); });

            auto index = match(
                operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                [&](OneOf<AStore0, DStore0, FStore0, IStore0, LStore0>) { return 0; },
                [&](OneOf<AStore1, DStore1, FStore1, IStore1, LStore1>) { return 1; },
                [&](OneOf<AStore2, DStore2, FStore2, IStore2, LStore2>) { return 2; },
                [&](OneOf<AStore3, DStore3, FStore3, IStore3, LStore3>) { return 3; });

            builder.CreateStore(operandStack.pop_back(type), locals[index]);
        },
        [&](AThrow)
        {
            llvm::Type* reference = referenceType(builder.getContext());

            llvm::Value* exception = operandStack.pop_back(reference);

            builder.CreateStore(exception, activeException(function->getParent()));

            builder.CreateBr(generateEHHandlerChain(exception, builder.GetInsertBlock()));
        },
        [&](BIPush biPush)
        {
            llvm::Value* res = builder.getInt32(biPush.value);
            operandStack.push_back(res);
        },
        // TODO: CheckCast
        [&](D2F)
        {
            llvm::Value* value = operandStack.pop_back(builder.getDoubleTy());
            operandStack.push_back(builder.CreateFPTrunc(value, builder.getFloatTy()));
        },
        [&](OneOf<D2I, D2L, F2I, F2L>)
        {
            auto [fromType, toType] = match(
                operation,
                [](...) -> std::tuple<llvm::Type*, llvm::Type*> { llvm_unreachable("Invalid conversion operation"); },
                [&](D2I) -> std::tuple<llvm::Type*, llvm::Type*> {
                    return {builder.getDoubleTy(), builder.getInt32Ty()};
                },
                [&](D2L) -> std::tuple<llvm::Type*, llvm::Type*> {
                    return {builder.getDoubleTy(), builder.getInt64Ty()};
                },
                [&](F2I) -> std::tuple<llvm::Type*, llvm::Type*> {
                    return {builder.getFloatTy(), builder.getInt32Ty()};
                },
                [&](F2L) -> std::tuple<llvm::Type*, llvm::Type*> {
                    return {builder.getFloatTy(), builder.getInt64Ty()};
                });

            llvm::Value* value = operandStack.pop_back(fromType);

            operandStack.push_back(builder.CreateIntrinsic(toType, llvm::Intrinsic::fptosi_sat, {value}));
        },
        [&](OneOf<DAdd, FAdd, IAdd, LAdd>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid add operation"); },
                [&](DAdd) { return builder.getDoubleTy(); }, [&](FAdd) { return builder.getFloatTy(); },
                [&](IAdd) { return builder.getInt32Ty(); }, [&](LAdd) { return builder.getInt64Ty(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

            auto* sum = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid add operation"); },
                [&](OneOf<DAdd, FAdd>) { return builder.CreateFAdd(lhs, rhs); },
                [&](OneOf<IAdd, LAdd>) { return builder.CreateAdd(lhs, rhs); });

            operandStack.push_back(sum);
        },
        [&](OneOf<DCmpG, DCmpL, FCmpG, FCmpL>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid comparison operation"); },
                [&](OneOf<DCmpG, DCmpL>) { return builder.getDoubleTy(); },
                [&](OneOf<FCmpG, FCmpL>) { return builder.getFloatTy(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

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
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid div operation"); },
                [&](DDiv) { return builder.getDoubleTy(); }, [&](FDiv) { return builder.getFloatTy(); },
                [&](IDiv) { return builder.getInt32Ty(); }, [&](LDiv) { return builder.getInt64Ty(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

            auto* quotient = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid div operation"); },
                [&](OneOf<DDiv, FDiv>) { return builder.CreateFDiv(lhs, rhs); },
                [&](OneOf<IDiv, LDiv>) { return builder.CreateSDiv(lhs, rhs); });

            operandStack.push_back(quotient);
        },
        [&](OneOf<DMul, FMul, IMul, LMul>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid mul operation"); },
                [&](DMul) { return builder.getDoubleTy(); }, [&](FMul) { return builder.getFloatTy(); },
                [&](IMul) { return builder.getInt32Ty(); }, [&](LMul) { return builder.getInt64Ty(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

            auto* product = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid mul operation"); },
                [&](OneOf<DMul, FMul>) { return builder.CreateFMul(lhs, rhs); },
                [&](OneOf<IMul, LMul>) { return builder.CreateMul(lhs, rhs); });

            operandStack.push_back(product);
        },
        [&](OneOf<DNeg, FNeg, INeg, LNeg>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid neg operation"); },
                [&](DNeg) { return builder.getDoubleTy(); }, [&](FNeg) { return builder.getFloatTy(); },
                [&](INeg) { return builder.getInt32Ty(); }, [&](LNeg) { return builder.getInt64Ty(); });

            llvm::Value* value = operandStack.pop_back(type);

            auto* result = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid neg operation"); },
                [&](OneOf<DNeg, FNeg>) { return builder.CreateFNeg(value); },
                [&](OneOf<INeg, LNeg>) { return builder.CreateNeg(value); });

            operandStack.push_back(result);
        },
        [&](OneOf<DRem, FRem, IRem, LRem>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid rem operation"); },
                [&](DRem) { return builder.getDoubleTy(); }, [&](FRem) { return builder.getFloatTy(); },
                [&](IRem) { return builder.getInt32Ty(); }, [&](LRem) { return builder.getInt64Ty(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

            auto* remainder = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid rem operation"); },
                [&](OneOf<DRem, FRem>) { return builder.CreateFRem(lhs, rhs); },
                [&](OneOf<IRem, LRem>) { return builder.CreateSRem(lhs, rhs); });

            operandStack.push_back(remainder);
        },
        [&](OneOf<DSub, FSub, ISub, LSub>)
        {
            auto* type = match(
                operation, [](...) -> llvm::Type* { llvm_unreachable("Invalid sub operation"); },
                [&](DSub) { return builder.getDoubleTy(); }, [&](FSub) { return builder.getFloatTy(); },
                [&](ISub) { return builder.getInt32Ty(); }, [&](LSub) { return builder.getInt64Ty(); });

            llvm::Value* rhs = operandStack.pop_back(type);
            llvm::Value* lhs = operandStack.pop_back(type);

            auto* difference = match(
                operation, [](...) -> llvm::Value* { llvm_unreachable("Invalid sub operation"); },
                [&](OneOf<DSub, FSub>) { return builder.CreateFSub(lhs, rhs); },
                [&](OneOf<ISub, LSub>) { return builder.CreateSub(lhs, rhs); });

            operandStack.push_back(difference);
        },
        [&](Dup)
        {
            llvm::Value* val = operandStack.pop_back(builder.getInt64Ty());
            operandStack.push_back(val);
            operandStack.push_back(val);
        },
        // TODO: DupX1
        // TODO: DupX2
        // TODO: Dup2
        // TODO: Dup2X1
        // TODO: Dup2X2
        [&](F2D)
        {
            llvm::Value* value = operandStack.pop_back(builder.getFloatTy());
            operandStack.push_back(builder.CreateFPExt(value, builder.getDoubleTy()));
        },
        [&](GetField getField)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{getField.index}.resolve(classFile);
            const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(classFile);
            FieldType descriptor = parseFieldType(nameAndTypeInfo->descriptorIndex.resolve(classFile)->text);
            llvm::Type* type = descriptorToType(descriptor, builder.getContext());

            llvm::Value* objectRef = operandStack.pop_back(referenceType(builder.getContext()));

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
            llvm::Value* fieldOffset = helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);
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
            const auto* refInfo = PoolIndex<FieldRefInfo>{getStatic.index}.resolve(classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;

            llvm::Value* fieldPtr = helper.getStaticFieldAddress(builder, className, fieldName, fieldType);
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
        [&](Goto gotoOp)
        {
            auto index = gotoOp.target + gotoOp.offset;
            basicBlockStackPointers.insert({basicBlocks[index], operandStack.getTopOfStack()});
            builder.CreateBr(basicBlocks[index]);
        },
        // TODO: GotoW
        [&](I2B)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt8Ty());
            operandStack.push_back(builder.CreateSExt(truncated, builder.getInt32Ty()));
        },
        [&](I2C)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
            operandStack.push_back(builder.CreateZExt(truncated, builder.getInt32Ty()));
        },
        [&](I2D)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateSIToFP(value, builder.getDoubleTy()));
        },
        [&](I2F)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateSIToFP(value, builder.getFloatTy()));
        },
        [&](I2L)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateSExt(value, builder.getInt64Ty()));
        },
        [&](I2S)
        {
            llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
            operandStack.push_back(builder.CreateSExt(truncated, builder.getInt32Ty()));
        },
        [&](IAnd)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
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
                [&](OneOf<IfACmpEq, IfACmpNe>)
                {
                    rhs = operandStack.pop_back(referenceType(builder.getContext()));
                    lhs = operandStack.pop_back(referenceType(builder.getContext()));
                },
                [&](OneOf<IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                {
                    rhs = operandStack.pop_back(builder.getInt32Ty());
                    lhs = operandStack.pop_back(builder.getInt32Ty());
                },
                [&](OneOf<IfEq, IfNe, IfLt, IfGe, IfGt, IfLe>)
                {
                    rhs = builder.getInt32(0);
                    lhs = operandStack.pop_back(builder.getInt32Ty());
                },
                [&](OneOf<IfNonNull, IfNull>)
                {
                    lhs = operandStack.pop_back(referenceType(builder.getContext()));
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
            basicBlockStackPointers.insert({basicBlock, operandStack.getTopOfStack()});
            basicBlockStackPointers.insert({next, operandStack.getTopOfStack()});
            builder.CreateCondBr(cond, basicBlock, next);
        },
        [&](IInc iInc)
        {
            llvm::Value* local = builder.CreateLoad(builder.getInt32Ty(), locals[iInc.index]);
            builder.CreateStore(builder.CreateAdd(local, builder.getInt32(iInc.byte)), locals[iInc.index]);
        },
        [&](InstanceOf instanceOf)
        {
            llvm::StringRef className =
                PoolIndex<ClassInfo>{instanceOf.index}.resolve(classFile)->nameIndex.resolve(classFile)->text;

            llvm::PointerType* ty = referenceType(builder.getContext());
            llvm::Value* object = operandStack.pop_back(ty);
            llvm::Value* null = llvm::ConstantPointerNull::get(ty);

            // null references always return 0.
            llvm::Value* isNull = builder.CreateICmpEQ(object, null);
            auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", function);
            auto* instanceOfBlock = llvm::BasicBlock::Create(builder.getContext(), "", function);
            llvm::BasicBlock* block = builder.GetInsertBlock();
            builder.CreateCondBr(isNull, continueBlock, instanceOfBlock);

            builder.SetInsertPoint(instanceOfBlock);

            llvm::Value* classObject;
            if (className.front() == '[')
            {
                // Weirdly, it uses normal field mangling if it's an array type, but for other class types it's
                // just the name of the class. Hence, these two cases.
                classObject = helper.getClassObject(builder, className);
            }
            else
            {
                classObject = helper.getClassObject(builder, "L" + className + ";");
            }
            // Can throw class loader or linkage related errors.
            generateEHDispatch();

            llvm::FunctionCallee callee = function->getParent()->getOrInsertFunction(
                "jllvm_instance_of", llvm::FunctionType::get(builder.getInt32Ty(), ty, classObject->getType()));
            llvm::Instruction* call = builder.CreateCall(callee, {object, classObject});
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(continueBlock);
            llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
            phi->addIncoming(builder.getInt32(0), block);
            phi->addIncoming(call, call->getParent());

            operandStack.push_back(phi);
        },
        // TODO: InvokeDynamic
        [&](InvokeInterface invokeInterface)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invokeInterface.index}.resolve(classFile);

            MethodType descriptor =
                parseMethodType(refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

            int i = descriptor.parameters.size() - 1;
            std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
            for (auto& iter : llvm::reverse(args))
            {
                iter =
                    operandStack.pop_back(i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                                   referenceType(builder.getContext()));
            }

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;

            llvm::Value* idAndSlot =
                helper.getITableIdAndOffset(builder, "L" + className + ";", methodName, methodType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(idAndSlot))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            std::size_t sizeTBits = std::numeric_limits<std::size_t>::digits;
            llvm::Value* slot = builder.CreateAnd(idAndSlot, builder.getIntN(sizeTBits, (1 << 8) - 1));
            llvm::Value* id = builder.CreateLShr(idAndSlot, builder.getIntN(sizeTBits, 8));

            llvm::Value* classObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
            llvm::Value* iTablesPtr = builder.CreateGEP(builder.getInt8Ty(), classObject,
                                                        {builder.getInt32(ClassObject::getITablesOffset())});
            llvm::Value* iTables =
                builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(arrayRefType(builder.getContext()), iTablesPtr,
                                                                         {builder.getInt32(0), builder.getInt32(0)}));

            // Linear search over all iTables of 'classObject' until the iTable with the interface id equal to
            // 'id' is found.
            auto* loopBody = llvm::BasicBlock::Create(builder.getContext(), "", function);
            llvm::BasicBlock* pred = builder.GetInsertBlock();
            builder.CreateBr(loopBody);

            builder.SetInsertPoint(loopBody);
            llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
            phi->addIncoming(builder.getInt32(0), pred);

            llvm::Value* iTable =
                builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(builder.getPtrTy(), iTables, {phi}));
            llvm::Value* iTableId = builder.CreateLoad(idAndSlot->getType(), iTable);
            llvm::Value* cond = builder.CreateICmpEQ(iTableId, id);
            llvm::Value* increment = builder.CreateAdd(phi, builder.getInt32(1));
            phi->addIncoming(increment, loopBody);

            auto* loopContinue = llvm::BasicBlock::Create(builder.getContext(), "", function);
            builder.CreateCondBr(cond, loopContinue, loopBody);

            builder.SetInsertPoint(loopContinue);

            llvm::Value* iTableSlot = builder.CreateGEP(iTableType(builder.getContext()), iTable,
                                                        {builder.getInt32(0), builder.getInt32(1), slot});
            llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), iTableSlot);

            llvm::FunctionType* functionType = descriptorToType(descriptor, false, builder.getContext());
            prepareArgumentsForCall(builder, args, functionType);
            auto* call = builder.CreateCall(functionType, callee, args);
            call->setAttributes(getABIAttributes(builder.getContext(), descriptor, /*isStatic=*/false));

            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                operandStack.push_back(call);
            }
        },
        [&](OneOf<InvokeSpecial, InvokeStatic> invoke)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invoke.index}.resolve(classFile);

            bool isStatic = holds_alternative<InvokeStatic>(operation);

            MethodType descriptor =
                parseMethodType(refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

            int i = descriptor.parameters.size() - 1;
            std::vector<llvm::Value*> args(descriptor.parameters.size() + (isStatic ? 0 : /*objectref*/ 1));
            for (auto& iter : llvm::reverse(args))
            {
                iter =
                    operandStack.pop_back(i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                                   referenceType(builder.getContext()));
            }

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
            llvm::Value* callee = helper.getNonVirtualCallee(builder, isStatic, className, methodName, methodType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(callee))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::FunctionType* functionType = descriptorToType(descriptor, isStatic, builder.getContext());
            prepareArgumentsForCall(builder, args, functionType);

            auto* call = builder.CreateCall(functionType, callee, args);
            call->setAttributes(getABIAttributes(builder.getContext(), descriptor, isStatic));

            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                operandStack.push_back(call);
            }
        },
        [&](InvokeVirtual invokeVirtual)
        {
            const RefInfo* refInfo = PoolIndex<RefInfo>{invokeVirtual.index}.resolve(classFile);

            MethodType descriptor =
                parseMethodType(refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

            int i = descriptor.parameters.size() - 1;
            std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
            for (auto& iter : llvm::reverse(args))
            {
                iter =
                    operandStack.pop_back(i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                                   referenceType(builder.getContext()));
            }
            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef methodType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
            llvm::Value* slot = helper.getVTableOffset(builder, "L" + className + ";", methodName, methodType);
            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(slot))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }
            llvm::Value* slotSize = builder.getInt16(sizeof(VTableSlot));
            llvm::Value* methodOffset = builder.CreateMul(slot, slotSize);
            llvm::Value* classObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
            llvm::Value* vtblPositionInClassObject = builder.getInt16(ClassObject::getVTableOffset());

            llvm::Value* totalOffset = builder.CreateAdd(vtblPositionInClassObject, methodOffset);
            llvm::Value* vtblSlot = builder.CreateGEP(builder.getInt8Ty(), classObject, {totalOffset});
            llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), vtblSlot);

            llvm::FunctionType* functionType = descriptorToType(descriptor, false, builder.getContext());
            prepareArgumentsForCall(builder, args, functionType);
            auto* call = builder.CreateCall(functionType, callee, args);
            call->setAttributes(getABIAttributes(builder.getContext(), descriptor, /*isStatic=*/false));

            generateEHDispatch();

            if (descriptor.returnType != FieldType(BaseType::Void))
            {
                operandStack.push_back(call);
            }
        },
        [&](IOr)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateOr(lhs, rhs));
        },
        [&](IShl)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* maskedRhs = builder.CreateAnd(
                rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateShl(lhs, maskedRhs));
        },
        [&](IShr)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* maskedRhs = builder.CreateAnd(
                rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateAShr(lhs, maskedRhs));
        },
        [&](IUShr)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* maskedRhs = builder.CreateAnd(
                rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateLShr(lhs, maskedRhs));
        },
        [&](IXor)
        {
            llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
            llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
            operandStack.push_back(builder.CreateXor(lhs, rhs));
        },
        // TODO: JSR
        // TODO: JSRw
        // TODO: L2D
        // TODO: L2F
        // TODO: L2I
        // TODO: LAnd
        // TODO: LCmp
        [&](OneOf<LDC, LDCW, LDC2W> ldc)
        {
            PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                      InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                pool{ldc.index};

            match(
                pool.resolve(classFile),
                [&](const IntegerInfo* integerInfo) { operandStack.push_back(builder.getInt32(integerInfo->value)); },
                [&](const FloatInfo* floatInfo)
                { operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), floatInfo->value)); },
                [&](const LongInfo* longInfo) { operandStack.push_back(builder.getInt64(longInfo->value)); },
                [&](const DoubleInfo* doubleInfo)
                { operandStack.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), doubleInfo->value)); },
                [&](const StringInfo* stringInfo)
                {
                    llvm::StringRef text = stringInfo->stringValue.resolve(classFile)->text;

                    String* string = stringInterner.intern(text);

                    operandStack.push_back(
                        builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(string)),
                                               referenceType(builder.getContext())));
                },
                [&](const ClassInfo* classInfo)
                {
                    llvm::StringRef text = classInfo->nameIndex.resolve(classFile)->text;
                    llvm::Value* classObject;
                    if (text.front() == '[')
                    {
                        classObject = helper.getClassObject(builder, text);
                    }
                    else
                    {
                        classObject = helper.getClassObject(builder, "L" + text + ";");
                    }
                    // If the class was already loaded 'callee' is optimized to a constant and no exception may
                    // occur.
                    if (!llvm::isa<llvm::Constant>(classObject))
                    {
                        // Can throw class loader or linkage related errors.
                        generateEHDispatch();
                    }
                    operandStack.push_back(classObject);
                },
                [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
        },
        // TODO: LookupSwitch
        // TODO: LOr
        // TODO: LShl
        // TODO: LShr
        // TODO: LUShr
        // TODO: LXor
        [&](OneOf<MonitorEnter, MonitorExit>)
        {
            // Pop object as is required by the instruction.
            // TODO: If we ever care about multi threading, this would require lazily creating a mutex and
            //  (un)locking it.
            operandStack.pop_back(referenceType(builder.getContext()));
        },
        [&](MultiANewArray multiANewArray)
        {
            llvm::StringRef descriptor =
                PoolIndex<ClassInfo>{multiANewArray.index}.resolve(classFile)->nameIndex.resolve(classFile)->text;

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
                          [&] { return llvm::BasicBlock::Create(builder.getContext(), "start", function); });

            std::generate(loopEnds.rbegin(), loopEnds.rend(),
                          [&] { return llvm::BasicBlock::Create(builder.getContext(), "end", function); });

            std::generate(loopCounts.rbegin(), loopCounts.rend(),
                          // TODO: throw NegativeArraySizeException
                          [&] { return operandStack.pop_back(builder.getInt32Ty()); });

            std::generate(arrayClassObjects.begin(), arrayClassObjects.end(),
                          [&]
                          {
                              llvm::Value* classObject = helper.getClassObject(builder, descriptor);
                              descriptor = descriptor.drop_front();

                              return classObject;
                          });

            // If the class was already loaded 'callee' is optimized to a constant and no exception may occur.
            if (!llvm::isa<llvm::Constant>(arrayClassObjects[0]))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            llvm::BasicBlock* done = llvm::BasicBlock::Create(builder.getContext(), "done", function);

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
            llvm::StringRef className =
                PoolIndex<ClassInfo>{newOp.index}.resolve(classFile)->nameIndex.resolve(classFile)->text;

            llvm::Value* classObject = helper.getClassObject(builder, "L" + className + ";");
            // If the class was already loaded 'callee' is optimized to a constant and no exception may
            // occur.
            if (!llvm::isa<llvm::Constant>(classObject))
            {
                // Can throw class loader or linkage related errors.
                generateEHDispatch();
            }

            // Size is first 4 bytes in the class object and does not include the object header.
            llvm::Value* fieldAreaPtr = builder.CreateGEP(builder.getInt8Ty(), classObject,
                                                          {builder.getInt32(ClassObject::getFieldAreaSizeOffset())});
            llvm::Value* size = builder.CreateLoad(builder.getInt32Ty(), fieldAreaPtr);
            size = builder.CreateAdd(size, builder.getInt32(sizeof(ObjectHeader)));

            llvm::Module* module = function->getParent();
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
            llvm::Value* count = operandStack.pop_back(builder.getInt32Ty());

            llvm::Value* classObject = helper.getClassObject(builder, "[" + descriptor);
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
            llvm::Value* object = builder.CreateCall(allocationFunction(function->getParent()), bytesNeeded);

            // Allocation can throw OutOfMemoryException.
            generateEHDispatch();

            builder.CreateStore(classObject, object);
            // Array length.
            llvm::Value* gep =
                builder.CreateGEP(arrayStructType(type), object, {builder.getInt32(0), builder.getInt32(1)});
            builder.CreateStore(count, gep);

            operandStack.push_back(object);
        },
        // TODO: Nop
        [&](Pop)
        {
            // Type does not matter as we do not use the result
            operandStack.pop_back(referenceType(builder.getContext()));
        },
        // TODO: Pop2
        [&](PutField putField)
        {
            const auto* refInfo = PoolIndex<FieldRefInfo>{putField.index}.resolve(classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
            llvm::Value* value = operandStack.pop_back(ensureI32(llvmFieldType, builder));
            llvm::Value* objectRef = operandStack.pop_back(referenceType(builder.getContext()));
            llvm::Value* fieldOffset = helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);
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
            const auto* refInfo = PoolIndex<FieldRefInfo>{putStatic.index}.resolve(classFile);

            llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldName =
                refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
            llvm::StringRef fieldType =
                refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
            llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
            llvm::Value* value = operandStack.pop_back(ensureI32(llvmFieldType, builder));
            llvm::Value* fieldPtr = helper.getStaticFieldAddress(builder, className, fieldName, fieldType);
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
        [&](SIPush siPush) { operandStack.push_back(builder.getInt32(siPush.value)); }
        // TODO: Swap
        // TODO: TableSwitch
        // TODO: Wide
    );
}

} // namespace

void jllvm::ByteCodeCompileLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const MethodInfo* methodInfo, const ClassFile* classFile)
{
    std::string methodName = mangleMethod(*methodInfo, *classFile);
    LLVM_DEBUG({ llvm::dbgs() << "Emitting LLVM IR for " << methodName << '\n'; });

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(methodName, *context);

    MethodType descriptor = parseMethodType(methodInfo->getDescriptor(*classFile));

    auto* function =
        llvm::Function::Create(descriptorToType(descriptor, methodInfo->isStatic(), module->getContext()),
                               llvm::GlobalValue::ExternalLinkage, mangleMethod(*methodInfo, *classFile), module.get());
    function->setGC("coreclr");
#ifdef LLVM_ADDRESS_SANITIZER_BUILD
    function->addFnAttr(llvm::Attribute::SanitizeAddress);
#endif

    auto code = methodInfo->getAttributes().find<Code>();
    assert(code);
    CodeGen codeGen(function, *code, *classFile,
                    LazyClassLoaderHelper(m_classLoader, m_mainDylib, m_stubsImplDylib, *m_stubsManager,
                                          m_callbackManager, m_baseLayer, m_interner, m_dataLayout),
                    m_stringInterner, descriptor);

    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

#ifndef NDEBUG
    if (llvm::verifyModule(*module, &llvm::dbgs()))
    {
        std::abort();
    }
#endif

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}
