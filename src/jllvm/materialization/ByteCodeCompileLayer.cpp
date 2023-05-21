#include "ByteCodeCompileLayer.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#include <jllvm/class/ByteCodeIterator.hpp>
#include <jllvm/class/Descriptors.hpp>
#include <jllvm/object/Object.hpp>
#include <jllvm/support/Bytes.hpp>

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
        const auto* baseType = std::get_if<BaseType>(&param);
        if (!baseType || !baseType->isIntegerType())
        {
            continue;
        }
        attrs = attrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }

    llvm::AttributeSet retAttrs;
    if (const auto* baseType = std::get_if<BaseType>(&methodType.returnType); baseType && baseType->isIntegerType())
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

void codeGenBody(llvm::Function* function, const Code& code, const ClassFile& classFile, LazyClassLoaderHelper helper,
                 StringInterner& stringInterner)
{
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(function->getContext(), "entry", function));
    OperandStack operandStack(code.getMaxStack(), builder);
    std::vector<llvm::AllocaInst*> locals(code.getMaxLocals());
    for (auto& alloca : locals)
    {
        // We need pointer size bytes, since that is the largest type we may store in a local.
        alloca = builder.CreateAlloca(llvm::PointerType::get(function->getContext(), 0));
    }

    // Arguments are put into the locals. According to the specification, i64s and doubles are split into two locals.
    // We don't actually do that, we just put them into the very first local, but we still have to skip over the
    // following local as if we didn't.
    auto nextLocal = locals.begin();
    for (auto& arg : function->args())
    {
        builder.CreateStore(&arg, *nextLocal++);
        if (arg.getType()->isIntegerTy(64) || arg.getType()->isDoubleTy())
        {
            nextLocal++;
        }
    }

    // TODO: Implement all the exception handling infrastructure.
    llvm::DenseMap<std::uint16_t, std::pair<std::uint16_t, PoolIndex<ClassInfo>>> startHandlers;
    llvm::DenseMap<std::uint16_t, std::pair<std::uint16_t, PoolIndex<ClassInfo>>> endHandlers;
    for (const auto& iter : code.getExceptionTable())
    {
        startHandlers.insert({iter.startPc, {iter.handlerPc, iter.catchType}});
        endHandlers.insert({iter.endPc, {iter.handlerPc, iter.catchType}});
    }

    std::vector<std::pair<std::uint16_t, PoolIndex<ClassInfo>>> activeHandlers;

    llvm::DenseMap<std::uint16_t, llvm::BasicBlock*> basicBlocks;
    // Calculate BasicBlocks
    for (auto [opCode, offset, current] : byteCodeRange(code.getCode()))
    {
        auto addBasicBlock = [&](std::uint16_t target)
        {
            auto [result, inserted] = basicBlocks.insert({target, nullptr});

            if (inserted)
            {
                result->second = llvm::BasicBlock::Create(builder.getContext(), "", function);
            }
        };
        switch (opCode)
        {
            case OpCodes::Goto:
            {
                auto target = consume<std::int16_t>(current);
                addBasicBlock(target + offset);
                break;
            }
            case OpCodes::GotoW:
            {
                auto target = consume<std::int32_t>(current);
                addBasicBlock(target + offset);
                break;
            }
            case OpCodes::IfACmpEq:
            case OpCodes::IfACmpNe:
            case OpCodes::IfICmpEq:
            case OpCodes::IfICmpNe:
            case OpCodes::IfICmpLt:
            case OpCodes::IfICmpGe:
            case OpCodes::IfICmpGt:
            case OpCodes::IfICmpLe:
            case OpCodes::IfEq:
            case OpCodes::IfNe:
            case OpCodes::IfLt:
            case OpCodes::IfGe:
            case OpCodes::IfGt:
            case OpCodes::IfLe:
            case OpCodes::IfNonNull:
            case OpCodes::IfNull:
            {
                auto target = consume<std::int16_t>(current);
                addBasicBlock(target + offset);
                addBasicBlock(current.data() - code.getCode().data());
                break;
            }
            default: break;
        }
    }
    llvm::DenseMap<llvm::BasicBlock*, llvm::AllocaInst**> basicBlockStackPointers;
    for (auto [opCode, offset, current] : byteCodeRange(code.getCode()))
    {
        if (auto result = startHandlers.find(offset); result != startHandlers.end())
        {
            activeHandlers.push_back(result->second);
        }
        if (auto result = endHandlers.find(offset); result != endHandlers.end())
        {
            auto iter = std::find(activeHandlers.rbegin(), activeHandlers.rend(), result->second);
            assert(iter != activeHandlers.rend());
            activeHandlers.erase(std::prev(iter.base()));
        }

        if (auto result = basicBlocks.find(offset); result != basicBlocks.end())
        {
            // Without any branches, there will not be a terminator at the end of the basic block. Thus, we need to set
            // this manually to the new insert point. This essentially implements implicit fallthrough from JVM
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
        switch (opCode)
        {
            default: llvm_unreachable("NOT YET IMPLEMENTED");
            case OpCodes::AALoad:
            {
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), gep));
                break;
            }
            case OpCodes::AAStore:
            {
                llvm::Value* value = operandStack.pop_back(referenceType(builder.getContext()));
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                builder.CreateStore(value, gep);

                break;
            }
            case OpCodes::AConstNull:
            {
                operandStack.push_back(llvm::ConstantPointerNull::get(referenceType(builder.getContext())));
                break;
            }
            case OpCodes::ALoad:
            {
                auto index = consume<std::uint8_t>(current);
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[index]));
                break;
            }
            case OpCodes::ALoad0:
            {
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[0]));
                break;
            }
            case OpCodes::ALoad1:
            {
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[1]));
                break;
            }
            case OpCodes::ALoad2:
            {
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[2]));
                break;
            }
            case OpCodes::ALoad3:
            {
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[3]));
                break;
            }
            case OpCodes::ANewArray:
            {
                auto index = consume<PoolIndex<ClassInfo>>(current);
                llvm::Value* count = operandStack.pop_back(builder.getInt32Ty());

                llvm::Value* classObject = helper.getClassObject(
                    builder, "[L" + index.resolve(classFile)->nameIndex.resolve(classFile)->text + ";");

                // Size required is the size of the array prior to the elements (equal to the offset to the elements)
                // plus element count * element size.
                llvm::Value* bytesNeeded = builder.getInt32(Array<>::arrayElementsOffset());
                bytesNeeded =
                    builder.CreateAdd(bytesNeeded, builder.CreateMul(count, builder.getInt32(sizeof(Object*))));

                // Type object.
                llvm::Value* object = builder.CreateCall(allocationFunction(function->getParent()), bytesNeeded);
                builder.CreateStore(classObject, object);
                // Array length.
                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), object,
                                              {builder.getInt32(0), builder.getInt32(1)});
                builder.CreateStore(count, gep);

                operandStack.push_back(object);
                break;
            }
            case OpCodes::AReturn:
            {
                llvm::Value* value = operandStack.pop_back(referenceType(builder.getContext()));
                builder.CreateRet(value);
                break;
            }
            case OpCodes::ArrayLength:
            {
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                // The element type of the array type here is actually irrelevant.
                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                              {builder.getInt32(0), builder.getInt32(1)});
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), gep));
                break;
            }
            case OpCodes::AStore:
            {
                auto index = consume<std::uint8_t>(current);
                builder.CreateStore(operandStack.pop_back(referenceType(builder.getContext())), locals[index]);
                break;
            }
            case OpCodes::AStore0:
            {
                builder.CreateStore(operandStack.pop_back(referenceType(builder.getContext())), locals[0]);
                break;
            }
            case OpCodes::AStore1:
            {
                builder.CreateStore(operandStack.pop_back(referenceType(builder.getContext())), locals[1]);
                break;
            }
            case OpCodes::AStore2:
            {
                builder.CreateStore(operandStack.pop_back(referenceType(builder.getContext())), locals[2]);
                break;
            }
            case OpCodes::AStore3:
            {
                builder.CreateStore(operandStack.pop_back(referenceType(builder.getContext())), locals[3]);
                break;
            }
            // TODO: AThrow
            // TODO: BALoad
            // TODO: BAStore
            case OpCodes::BIPush:
            {
                auto byte = consume<std::int8_t>(current);
                llvm::Value* res = builder.getInt32(byte);
                operandStack.push_back(res);
                break;
            }
            // TODO: CALoad
            // TODO: CAStore
            // TODO: CheckCast

            // TODO: D2F
            // TODO: D2I
            // TODO: D2L
            // TODO: DAdd
            // TODO: DALoad
            // TODO: DAStore
            // TODO: DCmpG
            // TODO: DCmpL
            // TODO: DConst0
            // TODO: DConst1
            // TODO: DDiv
            // TODO: DLoad
            // TODO: DLoad0
            // TODO: DLoad1
            // TODO: DLoad2
            // TODO: DLoad3
            // TODO: DMul
            // TODO: DNeg
            // TODO: DRem
            // TODO: DReturn
            // TODO: DStore
            // TODO: DStore0
            // TODO: DStore1
            // TODO: DStore2
            // TODO: DStore3
            // TODO: DSub
            case OpCodes::Dup:
            {
                llvm::Value* val = operandStack.pop_back(builder.getInt64Ty());
                operandStack.push_back(val);
                operandStack.push_back(val);
                break;
            }
            // TODO: DupX1
            // TODO: DupX2
            // TODO: Dup2
            // TODO: Dup2X1
            // TODO: Dup2X2

            // TODO: F2D
            // TODO: F2I
            // TODO: F2L
            case OpCodes::FAdd:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFAdd(lhs, rhs));
                break;
            }
            case OpCodes::FALoad:
            {
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                // TODO: throw NullPointerException if array is null
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
                auto* gep = builder.CreateGEP(arrayStructType(builder.getFloatTy()), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});

                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), gep));
                break;
            }
            case OpCodes::FAStore:
            {
                llvm::Value* value = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                // TODO: throw NullPointerException if array is null
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
                auto* gep = builder.CreateGEP(arrayStructType(builder.getFloatTy()), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                builder.CreateStore(value, gep);

                break;
            }
            case OpCodes::FCmpG:
            case OpCodes::FCmpL:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());

                // using unordered compare to allow for NaNs
                // if lhs == rhs result is 0, otherwise the resulting boolean is converted for the default case
                llvm::Value* notEqual = builder.CreateFCmpUNE(lhs, rhs);
                llvm::Value* otherCmp;
                llvm::Value* otherCase;

                switch (opCode)
                {
                    default: llvm_unreachable("Invalid comparison operation");
                    case OpCodes::FCmpG:
                    {
                        // is 0 if lhs == rhs, otherwise 1 for lhs > rhs or either operand being NaN
                        notEqual = builder.CreateZExt(notEqual, builder.getInt32Ty());
                        // using ordered less than to check lhs < rhs
                        otherCmp = builder.CreateFCmpOLT(lhs, rhs);
                        // return -1 if lhs < rhs
                        otherCase = builder.getInt32(-1);
                        break;
                    }
                    case OpCodes::FCmpL:
                    {
                        // is 0 if lhs == rhs, otherwise -1 for lhs < rhs or either operand being NaN
                        notEqual = builder.CreateSExt(notEqual, builder.getInt32Ty());
                        // using ordered greater than to check lhs > rhs
                        otherCmp = builder.CreateFCmpOGT(lhs, rhs);
                        // return -1 if lhs > rhs
                        otherCase = builder.getInt32(1);
                        break;
                    }
                }

                // select the non-default or the 0-or-default value based on the result of otherCmp
                operandStack.push_back(builder.CreateSelect(otherCmp, otherCase, notEqual));

                break;
            }
            case OpCodes::FConst0:
            {
                operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), 0.0));
                break;
            }
            case OpCodes::FConst1:
            {
                operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), 1.0));
                break;
            }
            case OpCodes::FConst2:
            {
                operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), 2.0));
                break;
            }
            case OpCodes::FDiv:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFDiv(lhs, rhs));
                break;
            }
            case OpCodes::FLoad:
            {
                auto index = consume<std::uint8_t>(current);
                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), locals[index]));
                break;
            }
            case OpCodes::FLoad0:
            {
                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), locals[0]));
                break;
            }
            case OpCodes::FLoad1:
            {
                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), locals[1]));
                break;
            }
            case OpCodes::FLoad2:
            {
                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), locals[2]));
                break;
            }
            case OpCodes::FLoad3:
            {
                operandStack.push_back(builder.CreateLoad(builder.getFloatTy(), locals[3]));
                break;
            }
            case OpCodes::FMul:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFMul(lhs, rhs));
                break;
            }
            case OpCodes::FNeg:
            {
                llvm::Value* value = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFNeg(value));
                break;
            }
            case OpCodes::FRem:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFRem(lhs, rhs));
                break;
            }
            case OpCodes::FReturn:
            {
                builder.CreateRet(operandStack.pop_back(builder.getFloatTy()));
                break;
            }
            case OpCodes::FStore:
            {
                auto index = consume<std::uint8_t>(current);
                builder.CreateStore(operandStack.pop_back(builder.getFloatTy()), locals[index]);
                break;
            }
            case OpCodes::FStore0:
            {
                builder.CreateStore(operandStack.pop_back(builder.getFloatTy()), locals[0]);
                break;
            }
            case OpCodes::FStore1:
            {
                builder.CreateStore(operandStack.pop_back(builder.getFloatTy()), locals[1]);
                break;
            }
            case OpCodes::FStore2:
            {
                builder.CreateStore(operandStack.pop_back(builder.getFloatTy()), locals[2]);
                break;
            }
            case OpCodes::FStore3:
            {
                builder.CreateStore(operandStack.pop_back(builder.getFloatTy()), locals[3]);
                break;
            }
            case OpCodes::FSub:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getFloatTy());
                llvm::Value* lhs = operandStack.pop_back(builder.getFloatTy());
                operandStack.push_back(builder.CreateFSub(lhs, rhs));
                break;
            }
            case OpCodes::GetField:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);
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

                llvm::Value* fieldPtr = builder.CreateGEP(builder.getInt8Ty(), objectRef, {fieldOffset});
                llvm::Value* field = builder.CreateLoad(type, fieldPtr);
                if (const auto* baseType = std::get_if<BaseType>(&descriptor);
                    baseType && baseType->getValue() < BaseType::Int)
                {
                    // Extend to the operands stack i32 type.
                    field = builder.CreateIntCast(field, builder.getInt32Ty(), /*isSigned=*/!baseType->isUnsigned());
                }

                operandStack.push_back(field);
                break;
            }
            case OpCodes::GetStatic:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;

                llvm::Value* fieldPtr = helper.getStaticFieldAddress(builder, className, fieldName, fieldType);
                FieldType descriptor = parseFieldType(fieldType);
                llvm::Type* type = descriptorToType(descriptor, builder.getContext());
                llvm::Value* field = builder.CreateLoad(type, fieldPtr);
                if (const auto* baseType = std::get_if<BaseType>(&descriptor);
                    baseType && baseType->getValue() < BaseType::Int)
                {
                    // Extend to the operands stack i32 type.
                    field = builder.CreateIntCast(field, builder.getInt32Ty(), /*isSigned=*/!baseType->isUnsigned());
                }
                operandStack.push_back(field);
                break;
            }
            case OpCodes::Goto:
            {
                auto target = consume<std::int16_t>(current);
                basicBlockStackPointers.insert({basicBlocks[target + offset], operandStack.getTopOfStack()});
                builder.CreateBr(basicBlocks[target + offset]);
                break;
            }
            case OpCodes::I2B:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt8Ty());
                operandStack.push_back(builder.CreateSExt(value, builder.getInt32Ty()));
                break;
            }
            case OpCodes::I2C:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
                operandStack.push_back(builder.CreateZExt(value, builder.getInt32Ty()));
                break;
            }
            case OpCodes::I2D:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSIToFP(value, builder.getDoubleTy()));
                break;
            }
            case OpCodes::I2F:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSIToFP(value, builder.getFloatTy()));
                break;
            }
            case OpCodes::I2L:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSExt(value, builder.getInt64Ty()));
                break;
            }
            case OpCodes::I2S:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* truncated = builder.CreateTrunc(value, builder.getInt16Ty());
                operandStack.push_back(builder.CreateSExt(value, builder.getInt32Ty()));
                break;
            }
            case OpCodes::IAdd:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateAdd(lhs, rhs));
                break;
            }
            case OpCodes::IALoad:
            {
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                // TODO: throw NullPointerException if array is null
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
                auto* gep = builder.CreateGEP(arrayStructType(builder.getInt32Ty()), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), gep));
                break;
            }
            case OpCodes::IAnd:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateAnd(lhs, rhs));
                break;
            }
            case OpCodes::IAStore:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* index = operandStack.pop_back(builder.getInt32Ty());
                // TODO: throw NullPointerException if array is null
                llvm::Value* array = operandStack.pop_back(referenceType(builder.getContext()));

                // TODO: throw ArrayIndexOutOfBoundsException if index is not within the bounds
                auto* gep = builder.CreateGEP(arrayStructType(builder.getInt32Ty()), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                builder.CreateStore(value, gep);

                break;
            }
            case OpCodes::IConstM1:
            {
                operandStack.push_back(builder.getInt32(-1));
                break;
            }
            case OpCodes::IConst0:
            {
                operandStack.push_back(builder.getInt32(0));
                break;
            }
            case OpCodes::IConst1:
            {
                operandStack.push_back(builder.getInt32(1));
                break;
            }
            case OpCodes::IConst2:
            {
                operandStack.push_back(builder.getInt32(2));
                break;
            }
            case OpCodes::IConst3:
            {
                operandStack.push_back(builder.getInt32(3));
                break;
            }
            case OpCodes::IConst4:
            {
                operandStack.push_back(builder.getInt32(4));
                break;
            }
            case OpCodes::IConst5:
            {
                operandStack.push_back(builder.getInt32(5));
                break;
            }
            case OpCodes::IDiv:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSDiv(lhs, rhs));
                break;
            }
            case OpCodes::IfACmpEq:
            case OpCodes::IfACmpNe:
            case OpCodes::IfICmpEq:
            case OpCodes::IfICmpNe:
            case OpCodes::IfICmpLt:
            case OpCodes::IfICmpGe:
            case OpCodes::IfICmpGt:
            case OpCodes::IfICmpLe:
            case OpCodes::IfEq:
            case OpCodes::IfNe:
            case OpCodes::IfLt:
            case OpCodes::IfGe:
            case OpCodes::IfGt:
            case OpCodes::IfLe:
            {
                auto target = consume<std::int16_t>(current);
                llvm::BasicBlock* basicBlock = basicBlocks[target + offset];
                llvm::BasicBlock* next = basicBlocks[current.data() - code.getCode().data()];

                llvm::Value* rhs;
                llvm::Value* lhs;
                llvm::CmpInst::Predicate predicate;

                switch (opCode)
                {
                    default: llvm_unreachable("Invalid comparison operation");
                    case OpCodes::IfACmpEq:
                    case OpCodes::IfACmpNe:
                    {
                        rhs = operandStack.pop_back(referenceType(builder.getContext()));
                        lhs = operandStack.pop_back(referenceType(builder.getContext()));
                        break;
                    }
                    case OpCodes::IfICmpEq:
                    case OpCodes::IfICmpNe:
                    case OpCodes::IfICmpLt:
                    case OpCodes::IfICmpGe:
                    case OpCodes::IfICmpGt:
                    case OpCodes::IfICmpLe:
                    {
                        rhs = operandStack.pop_back(builder.getInt32Ty());
                        lhs = operandStack.pop_back(builder.getInt32Ty());
                        break;
                    }
                    case OpCodes::IfEq:
                    case OpCodes::IfNe:
                    case OpCodes::IfLt:
                    case OpCodes::IfGe:
                    case OpCodes::IfGt:
                    case OpCodes::IfLe:
                    {
                        rhs = builder.getInt32(0);
                        lhs = operandStack.pop_back(builder.getInt32Ty());
                        break;
                    }
                }

                switch (opCode)
                {
                    default: llvm_unreachable("Invalid comparison operation");
                    case OpCodes::IfACmpEq:
                    case OpCodes::IfICmpEq:
                    case OpCodes::IfEq:
                    {
                        predicate = llvm::CmpInst::ICMP_EQ;
                        break;
                    }
                    case OpCodes::IfACmpNe:
                    case OpCodes::IfICmpNe:
                    case OpCodes::IfNe:
                    {
                        predicate = llvm::CmpInst::ICMP_NE;
                        break;
                    }
                    case OpCodes::IfICmpLt:
                    case OpCodes::IfLt:
                    {
                        predicate = llvm::CmpInst::ICMP_SLT;
                        break;
                    }
                    case OpCodes::IfICmpLe:
                    case OpCodes::IfLe:
                    {
                        predicate = llvm::CmpInst::ICMP_SLE;
                        break;
                    }
                    case OpCodes::IfICmpGt:
                    case OpCodes::IfGt:
                    {
                        predicate = llvm::CmpInst::ICMP_SGT;
                        break;
                    }
                    case OpCodes::IfICmpGe:
                    case OpCodes::IfGe:
                    {
                        predicate = llvm::CmpInst::ICMP_SGE;
                        break;
                    }
                }

                llvm::Value* cond = builder.CreateICmp(predicate, lhs, rhs);
                basicBlockStackPointers.insert({basicBlock, operandStack.getTopOfStack()});
                basicBlockStackPointers.insert({next, operandStack.getTopOfStack()});
                builder.CreateCondBr(cond, basicBlock, next);

                break;
            }
            case OpCodes::IInc:
            {
                auto index = consume<std::uint8_t>(current);
                auto constant = consume<std::int8_t>(current);

                llvm::Value* local = builder.CreateLoad(builder.getInt32Ty(), locals[index]);
                builder.CreateStore(builder.CreateAdd(local, builder.getInt32(constant)), locals[index]);

                break;
            }
            case OpCodes::ILoad:
            {
                auto index = consume<std::uint8_t>(current);
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), locals[index]));
                break;
            }
            case OpCodes::ILoad0:
            {
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), locals[0]));
                break;
            }
            case OpCodes::ILoad1:
            {
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), locals[1]));
                break;
            }
            case OpCodes::ILoad2:
            {
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), locals[2]));
                break;
            }
            case OpCodes::ILoad3:
            {
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), locals[3]));
                break;
            }
            case OpCodes::IMul:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateMul(lhs, rhs));
                break;
            }
            case OpCodes::INeg:
            {
                llvm::Value* value = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateNeg(value));
                break;
            }
            case OpCodes::InstanceOf:
            {
                llvm::StringRef className =
                    consume<PoolIndex<ClassInfo>>(current).resolve(classFile)->nameIndex.resolve(classFile)->text;

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
                    // Weirdly, it uses normal field mangling if it's an array type, but for other class types it's just
                    // the name of the class. Hence, these two cases.
                    classObject = helper.getClassObject(builder, className);
                }
                else
                {
                    classObject = helper.getClassObject(builder, "L" + className + ";");
                }

                llvm::FunctionCallee callee = function->getParent()->getOrInsertFunction(
                    "jllvm_instance_of", llvm::FunctionType::get(builder.getInt32Ty(), ty, classObject->getType()));
                llvm::Value* call = builder.CreateCall(callee, {object, classObject});
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(continueBlock);
                llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
                phi->addIncoming(builder.getInt32(0), block);
                phi->addIncoming(call, instanceOfBlock);

                operandStack.push_back(phi);
                break;
            }
            // TODO: InvokeDynamic
            case OpCodes::InvokeInterface:
            {
                const RefInfo* refInfo = consume<PoolIndex<RefInfo>>(current).resolve(classFile);
                // Legacy bytes that have become unused.
                consume<std::uint8_t>(current);
                consume<std::uint8_t>(current);

                MethodType descriptor = parseMethodType(
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

                int i = descriptor.parameters.size() - 1;
                std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
                for (auto& iter : llvm::reverse(args))
                {
                    iter = operandStack.pop_back(
                        i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                 referenceType(builder.getContext()));
                }

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;

                llvm::Value* idAndSlot =
                    helper.getITableIdAndOffset(builder, "L" + className + ";", methodName, methodType);

                std::size_t sizeTBits = std::numeric_limits<std::size_t>::digits;
                llvm::Value* slot = builder.CreateAnd(idAndSlot, builder.getIntN(sizeTBits, (1 << 8) - 1));
                llvm::Value* id = builder.CreateLShr(idAndSlot, builder.getIntN(sizeTBits, 8));

                llvm::Value* classObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
                llvm::Value* iTablesPtr = builder.CreateGEP(builder.getInt8Ty(), classObject,
                                                            {builder.getInt32(ClassObject::getITablesOffset())});
                llvm::Value* iTables = builder.CreateLoad(
                    builder.getPtrTy(), builder.CreateGEP(arrayRefType(builder.getContext()), iTablesPtr,
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

                if (descriptor.returnType != FieldType(BaseType::Void))
                {
                    operandStack.push_back(call);
                }

                break;
            }
            case OpCodes::InvokeSpecial:
            case OpCodes::InvokeStatic:
            {
                const RefInfo* refInfo = consume<PoolIndex<RefInfo>>(current).resolve(classFile);

                bool isStatic = opCode == OpCodes::InvokeStatic;

                MethodType descriptor = parseMethodType(
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

                int i = descriptor.parameters.size() - 1;
                std::vector<llvm::Value*> args(descriptor.parameters.size() + (isStatic ? 0 : /*objectref*/ 1));
                for (auto& iter : llvm::reverse(args))
                {
                    iter = operandStack.pop_back(
                        i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                 referenceType(builder.getContext()));
                }

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Value* callee = helper.getNonVirtualCallee(builder, isStatic, className, methodName, methodType);

                llvm::FunctionType* functionType = descriptorToType(descriptor, isStatic, builder.getContext());
                prepareArgumentsForCall(builder, args, functionType);

                auto* call = builder.CreateCall(functionType, callee, args);
                call->setAttributes(getABIAttributes(builder.getContext(), descriptor, isStatic));

                if (descriptor.returnType != FieldType(BaseType::Void))
                {
                    operandStack.push_back(call);
                }

                break;
            }
            case OpCodes::InvokeVirtual:
            {
                const RefInfo* refInfo = consume<PoolIndex<RefInfo>>(current).resolve(classFile);

                MethodType descriptor = parseMethodType(
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

                int i = descriptor.parameters.size() - 1;
                std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
                for (auto& iter : llvm::reverse(args))
                {
                    iter = operandStack.pop_back(
                        i >= 0 ? descriptorToType(descriptor.parameters[i--], builder.getContext()) :
                                 referenceType(builder.getContext()));
                }
                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef methodType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Value* slot = helper.getVTableOffset(builder, "L" + className + ";", methodName, methodType);
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

                if (descriptor.returnType != FieldType(BaseType::Void))
                {
                    operandStack.push_back(call);
                }
                break;
            }
            case OpCodes::IOr:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateOr(lhs, rhs));
                break;
            }
            case OpCodes::IRem:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSRem(lhs, rhs));
                break;
            }
            case OpCodes::IReturn:
            {
                builder.CreateRet(operandStack.pop_back(builder.getInt32Ty()));
                break;
            }
            case OpCodes::IShl:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* maskedRhs = builder.CreateAnd(
                    rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateShl(lhs, maskedRhs));
                break;
            }
            case OpCodes::IShr:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* maskedRhs = builder.CreateAnd(
                    rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateAShr(lhs, maskedRhs));
                break;
            }
            case OpCodes::IStore:
            {
                auto index = consume<std::uint8_t>(current);
                builder.CreateStore(operandStack.pop_back(builder.getInt32Ty()), locals[index]);
                break;
            }
            case OpCodes::IStore0:
            {
                builder.CreateStore(operandStack.pop_back(builder.getInt32Ty()), locals[0]);
                break;
            }
            case OpCodes::IStore1:
            {
                builder.CreateStore(operandStack.pop_back(builder.getInt32Ty()), locals[1]);
                break;
            }
            case OpCodes::IStore2:
            {
                builder.CreateStore(operandStack.pop_back(builder.getInt32Ty()), locals[2]);
                break;
            }
            case OpCodes::IStore3:
            {
                builder.CreateStore(operandStack.pop_back(builder.getInt32Ty()), locals[3]);
                break;
            }
            case OpCodes::ISub:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateSub(lhs, rhs));
                break;
            }
            case OpCodes::IUShr:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* maskedRhs = builder.CreateAnd(
                    rhs, builder.getInt32(0x1F)); // According to JVM only the lower 5 bits shall be considered
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateLShr(lhs, maskedRhs));
                break;
            }
            case OpCodes::IXor:
            {
                llvm::Value* rhs = operandStack.pop_back(builder.getInt32Ty());
                llvm::Value* lhs = operandStack.pop_back(builder.getInt32Ty());
                operandStack.push_back(builder.CreateXor(lhs, rhs));
                break;
            }
            // TODO: JSR
            // TODO: JSRw
            // TODO: L2D
            // TODO: L2F
            // TODO: L2I
            // TODO: LAdd
            // TODO: LALoad
            // TODO: LAnd
            // TODO: LAStore
            // TODO: LCmp
            // TODO: LConst0
            // TODO: LConst1
            case OpCodes::LDC:
            {
                auto index = consume<std::uint8_t>(current);
                PoolIndex<IntegerInfo, FloatInfo, StringInfo, MethodRefInfo, InterfaceMethodRefInfo, ClassInfo,
                          MethodTypeInfo, DynamicInfo>
                    pool(index);

                match(
                    pool.resolve(classFile),
                    [&](const IntegerInfo* integerInfo)
                    { operandStack.push_back(builder.getInt32(integerInfo->value)); },
                    [&](const FloatInfo* floatInfo)
                    { operandStack.push_back(llvm::ConstantFP::get(builder.getFloatTy(), floatInfo->value)); },
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
                        operandStack.push_back(classObject);
                    },
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });

                break;
            }
            // TODO: LDCW
            // TODO: LDC2W
            // TODO: LDiv
            // TODO: LLoad
            // TODO: LLoad0
            // TODO: LLoad1
            // TODO: LLoad2
            // TODO: LLoad3
            // TODO: LMul
            // TODO: LNeg
            // TODO: LookupSwitch
            // TODO: LOr
            // TODO: LRem
            // TODO: LReturn
            // TODO: LShl
            // TODO: LShr
            // TODO: LStore
            // TODO: LStore0
            // TODO: LStore1
            // TODO: LStore2
            // TODO: LStore3
            // TODO: LSub
            // TODO: LUShr
            // TODO: LXor
            // TODO: MonitorEnter
            // TODO: MonitorExit
            // TODO: MultiANewArray
            case OpCodes::New:
            {
                llvm::StringRef className =
                    consume<PoolIndex<ClassInfo>>(current).resolve(classFile)->nameIndex.resolve(classFile)->text;

                llvm::Value* classObject = helper.getClassObject(builder, "L" + className + ";");

                // Size is first 4 bytes in the class object and does not include the object header.
                llvm::Value* fieldAreaPtr = builder.CreateGEP(
                    builder.getInt8Ty(), classObject, {builder.getInt32(ClassObject::getFieldAreaSizeOffset())});
                llvm::Value* size = builder.CreateLoad(builder.getInt32Ty(), fieldAreaPtr);
                size = builder.CreateAdd(size, builder.getInt32(sizeof(ObjectHeader)));

                llvm::Module* module = function->getParent();
                llvm::Value* object = builder.CreateCall(allocationFunction(module), size);
                // Store object header (which in our case is just the class object) in the object.
                builder.CreateStore(classObject, object);
                operandStack.push_back(object);

                break;
            }
            case OpCodes::NewArray:
            {
                enum class ArrayType : std::uint8_t
                {
                    TBoolean = 4,
                    TChar = 5,
                    TFloat = 6,
                    TDouble = 7,
                    TByte = 8,
                    TShort = 9,
                    TInt = 10,
                    TLong = 11
                };

                auto type = consume<ArrayType>(current);
                llvm::Value* count = operandStack.pop_back(builder.getInt32Ty());

                auto resolveTypeDescriptor = [](ArrayType type) -> llvm::StringRef
                {
                    switch (type)
                    {
                        case ArrayType::TBoolean: return "Z";
                        case ArrayType::TChar: return "C";
                        case ArrayType::TFloat: return "F";
                        case ArrayType::TDouble: return "D";
                        case ArrayType::TByte: return "B";
                        case ArrayType::TShort: return "S";
                        case ArrayType::TInt: return "I";
                        default: return "J";
                    }
                };

                auto resolveTypeSize = [](ArrayType type)
                {
                    switch (type)
                    {
                        case ArrayType::TBoolean:
                        case ArrayType::TChar:
                        case ArrayType::TByte: return sizeof(std::uint8_t);
                        case ArrayType::TShort: return sizeof(std::uint16_t);
                        case ArrayType::TInt: return sizeof(std::uint32_t);
                        case ArrayType::TFloat: return sizeof(float);
                        case ArrayType::TDouble: return sizeof(double);
                        default: return sizeof(std::uint64_t);
                    }
                };

                auto resolveElementType = [&builder](ArrayType type) -> llvm::Type*
                {
                    switch (type)
                    {
                        case ArrayType::TBoolean:
                        case ArrayType::TChar:
                        case ArrayType::TByte: return builder.getInt8Ty();
                        case ArrayType::TShort: return builder.getInt16Ty();
                        case ArrayType::TInt: return builder.getInt32Ty();
                        case ArrayType::TFloat: return builder.getFloatTy();
                        case ArrayType::TDouble: return builder.getDoubleTy();
                        default: return builder.getInt64Ty();
                    }
                };

                llvm::Value* classObject = helper.getClassObject(builder, "[" + resolveTypeDescriptor(type));

                // Size required is the size of the array prior to the elements (equal to the offset to the elements)
                // plus element count * element size.
                llvm::Value* bytesNeeded = builder.getInt32(Array<>::arrayElementsOffset());
                bytesNeeded =
                    builder.CreateAdd(bytesNeeded, builder.CreateMul(count, builder.getInt32(resolveTypeSize(type))));

                // Type object.
                llvm::Value* object = builder.CreateCall(allocationFunction(function->getParent()), bytesNeeded);
                builder.CreateStore(classObject, object);
                // Array length.
                auto* gep = builder.CreateGEP(arrayStructType(resolveElementType(type)), object,
                                              {builder.getInt32(0), builder.getInt32(1)});
                builder.CreateStore(count, gep);

                operandStack.push_back(object);
                break;
            }
            // TODO: Nop
            case OpCodes::Pop:
            {
                // Type does not matter as we do not use the result
                operandStack.pop_back(referenceType(builder.getContext()));
                break;
            }
            // TODO: Pop2
            case OpCodes::PutField:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
                llvm::Value* value = operandStack.pop_back(ensureI32(llvmFieldType, builder));
                llvm::Value* objectRef = operandStack.pop_back(referenceType(builder.getContext()));
                llvm::Value* fieldOffset = helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);

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
                break;
            }
            case OpCodes::PutStatic:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
                llvm::Value* value = operandStack.pop_back(ensureI32(llvmFieldType, builder));
                llvm::Value* fieldPtr = helper.getStaticFieldAddress(builder, className, fieldName, fieldType);

                if (value->getType() != llvmFieldType)
                {
                    // Truncated from the operands stack i32 type.
                    assert(value->getType()->isIntegerTy() && llvmFieldType->isIntegerTy()
                           && value->getType()->getIntegerBitWidth() > llvmFieldType->getIntegerBitWidth());
                    value = builder.CreateTrunc(value, llvmFieldType);
                }

                builder.CreateStore(value, fieldPtr);
                break;
            }
            // TODO: Ret
            case OpCodes::Return:
            {
                builder.CreateRetVoid();
                break;
            }
            // TODO: SALoad
            // TODO: SAStore
            case OpCodes::SIPush:
            {
                auto value = consume<std::int16_t>(current);
                operandStack.push_back(builder.getInt32(value));
                break;
            }
                // TODO: Swap
                // TODO: TableSwitch
                // TODO: Wide
        }
    }
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
    auto code = methodInfo->getAttributes().find<Code>();
    assert(code);
    codeGenBody(function, *code, *classFile,
                LazyClassLoaderHelper(m_classLoader, m_mainDylib, m_stubsImplDylib, *m_stubsManager, m_callbackManager,
                                      m_baseLayer, m_interner, m_dataLayout),
                m_stringInterner);

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
