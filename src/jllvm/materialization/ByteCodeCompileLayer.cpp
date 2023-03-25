#include "ByteCodeCompileLayer.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Debug.h>

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

auto arrayStructType(llvm::Type* elementType)
{
    return llvm::StructType::get(elementType->getContext(), {referenceType(elementType->getContext()),
                                                             llvm::Type::getInt32Ty(elementType->getContext()),
                                                             llvm::ArrayType::get(elementType, 0)});
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

/// X86 ABI essentially always uses the 32 bit register names for passing along integers. Using the 'signext' attribute
/// we tell LLVM that if due to ABI, it has to extend these registers, to use sign extension.
/// This attribute list can be applied to either a call or a function itself.
llvm::AttributeList getABIAttributes(llvm::FunctionType* functionType)
{
    llvm::SmallVector<llvm::AttributeSet> paramAttrs(functionType->getNumParams());
    for (auto&& [param, attrs] : llvm::zip(functionType->params(), paramAttrs))
    {
        if (!param->isIntegerTy())
        {
            continue;
        }
        attrs = attrs.addAttribute(functionType->getContext(), llvm::Attribute::SExt);
    }

    llvm::AttributeSet retAttrs;
    if (functionType->getReturnType()->isIntegerTy())
    {
        retAttrs = retAttrs.addAttribute(functionType->getContext(), llvm::Attribute::SExt);
    }

    return llvm::AttributeList::get(functionType->getContext(), llvm::AttributeSet{}, retAttrs, paramAttrs);
}

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
    llvm::Value* returnConstantForClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor, llvm::Twine key, F&& f)
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

        std::string stubSymbol =
            ("<classLoad>" + fieldDescriptor + key)
                .str();
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
        return returnConstantForClassObject(
            builder, "L" + className + ";", fieldName + ";" + fieldType,
                                                [=](const ClassObject* classObject)
            {
                // TODO: Pretty sure this needs to also go through the inheritance hierarchy among other details.
                const Field* iter = llvm::find_if(
                    classObject->getFields(), [&](const Field& field)
                    { return !field.isStatic() && field.getName() == fieldName && field.getType() == fieldType; });
                assert(iter != classObject->getFields().end());
                return iter->getOffset();
            });
    }

    llvm::Value* getVTableOffset(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor, llvm::StringRef methodName,
                               llvm::StringRef typeDescriptor)
    {
        return returnConstantForClassObject(builder, fieldDescriptor, methodName + ";" + typeDescriptor,
                                            [=](const ClassObject* classObject)
                                            {
                                                const Method* iter;
                                                do
                                                {
                                                    llvm::ArrayRef<Method> methods = classObject->getMethods();
                                                   iter =
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
                                                    classObject = classObject->getSuperClass();
                                                } while (classObject != nullptr);
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
            {
                // TODO: Pretty sure this needs to also go through the inheritance hierarchy among other details.
                const Field* iter = llvm::find_if(
                    classObject->getFields(), [&](const Field& field)
                    { return field.isStatic() && field.getName() == fieldName && field.getType() == fieldType; });
                assert(iter != classObject->getFields().end());
                return iter->getAddressOfStatic();
            });
    }

    /// Returns an LLVM Pointer which points to the class object of the type with the given field descriptor.
    llvm::Value* getClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor)
    {
        return returnConstantForClassObject(builder, fieldDescriptor, "",
                                            [=](const ClassObject* classObject) { return classObject; });
    }
};

enum class OpCodes : std::uint8_t
{
    AALoad = 0x32,
    AAStore = 0x53,
    AConstNull = 0x1,
    ALoad = 0x19,
    ALoad0 = 0x2a,
    ALoad1 = 0x2b,
    ALoad2 = 0x2c,
    ALoad3 = 0x2d,
    ANewArray = 0xbd,
    AReturn = 0xb0,
    ArrayLength = 0xbe,
    AStore = 0x3a,
    AStore0 = 0x4b,
    AStore1 = 0x4c,
    AStore2 = 0x4d,
    AStore3 = 0x4e,
    AThrow = 0xbf,
    BALoad = 0x33,
    BAStore = 0x54,
    BIPush = 0x10,
    CALoad = 0x34,
    CAStore = 0x55,
    CheckCast = 0xc0,
    D2F = 0x90,
    D2I = 0x8e,
    D2L = 0x8f,
    DAdd = 0x63,
    DALoad = 0x31,
    DAStore = 0x52,
    DCmpG = 0x98,
    DCmpL = 0x97,
    DConst0 = 0xe,
    DConst1 = 0xf,
    DDiv = 0x6f,
    DLoad = 0x18,
    DLoad0 = 0x26,
    DLoad1 = 0x27,
    DLoad2 = 0x28,
    DLoad3 = 0x29,
    DMul = 0x6b,
    DNeg = 0x77,
    DRem = 0x73,
    DReturn = 0xaf,
    DStore = 0x39,
    DStore0 = 0x47,
    DStore1 = 0x48,
    DStore2 = 0x49,
    DStore3 = 0x4a,
    DSub = 0x67,
    Dup = 0x59,
    DupX1 = 0x5a,
    DupX2 = 0x5b,
    Dup2 = 0x5c,
    Dup2X1 = 0x5d,
    Dup2X2 = 0x5e,
    F2D = 0x8d,
    F2I = 0x8b,
    F2L = 0x8c,
    FAdd = 0x62,
    FALoad = 0x30,
    FAStore = 0x51,
    FCmpG = 0x96,
    FCmpL = 0x95,
    FConst0 = 0xb,
    FConst1 = 0xc,
    FConst2 = 0xd,
    FDiv = 0x6e,
    FLoad = 0x17,
    FLoad0 = 0x22,
    FLoad1 = 0x23,
    FLoad2 = 0x24,
    FLoad3 = 0x25,
    FMul = 0x6a,
    FNeg = 0x76,
    FRem = 0x72,
    FReturn = 0xae,
    FStore = 0x38,
    FStore0 = 0x43,
    FStore1 = 0x44,
    FStore2 = 0x45,
    FStore3 = 0x46,
    FSub = 0x66,
    GetField = 0xb4,
    GetStatic = 0xb2,
    Goto = 0xa7,
    GotoW = 0xc8,
    I2B = 0x91,
    I2C = 0x92,
    I2D = 0x87,
    I2F = 0x86,
    I2L = 0x85,
    I2S = 0x93,
    IAdd = 0x60,
    IALoad = 0x2e,
    IAnd = 0x7e,
    IAStore = 0x4f,
    IConstM1 = 0x2,
    IConst0 = 0x3,
    IConst1 = 0x4,
    IConst2 = 0x5,
    IConst3 = 0x6,
    IConst4 = 0x7,
    IConst5 = 0x8,
    IDiv = 0x6c,
    IfACmpEq = 0xa5,
    IfACmpNe = 0xa6,
    IfICmpEq = 0x9f,
    IfICmpNe = 0xa0,
    IfICmpLt = 0xa1,
    IfICmpGe = 0xa2,
    IfICmpGt = 0xa3,
    IfICmpLe = 0xa4,
    IfEq = 0x99,
    IfNe = 0x9a,
    IfLt = 0x9b,
    IfGe = 0x9c,
    IfGt = 0x9d,
    IfLe = 0x9e,
    IfNonNull = 0xc7,
    IfNull = 0xc6,
    IInc = 0x84,
    ILoad = 0x15,
    ILoad0 = 0x1a,
    ILoad1 = 0x1b,
    ILoad2 = 0x1c,
    ILoad3 = 0x1d,
    IMul = 0x68,
    INeg = 0x74,
    InstanceOf = 0xc1,
    InvokeDynamic = 0xba,
    InvokeInterface = 0xb9,
    InvokeSpecial = 0xb7,
    InvokeStatic = 0xb8,
    InvokeVirtual = 0xb6,
    IOr = 0x80,
    IRem = 0x70,
    IReturn = 0xac,
    IShl = 0x78,
    IShr = 0x7a,
    IStore = 0x36,
    IStore0 = 0x3b,
    IStore1 = 0x3c,
    IStore2 = 0x3d,
    IStore3 = 0x3e,
    ISub = 0x64,
    IUShr = 0x7c,
    IXor = 0x82,
    JSR = 0xa8,
    JSRw = 0xc9,
    L2D = 0x8a,
    L2F = 0x89,
    L2I = 0x88,
    LAdd = 0x61,
    LALoad = 0x2f,
    LAnd = 0x7f,
    LAStore = 0x50,
    LCmp = 0x94,
    LConst0 = 0x9,
    LConst1 = 0xa,
    LDC = 0x12,
    LDCW = 0x13,
    LDC2W = 0x14,
    LDiv = 0x6d,
    LLoad = 0x16,
    LLoad0 = 0x1e,
    LLoad1 = 0x1f,
    LLoad2 = 0x20,
    LLoad3 = 0x21,
    LMul = 0x69,
    LNeg = 0x75,
    LookupSwitch = 0xab,
    LOr = 0x81,
    LRem = 0x71,
    LReturn = 0xad,
    LShl = 0x79,
    LShr = 0x7b,
    LStore = 0x37,
    LStore0 = 0x3f,
    LStore1 = 0x40,
    LStore2 = 0x41,
    LStore3 = 0x42,
    LSub = 0x65,
    LUShr = 0x7d,
    LXor = 0x83,
    MonitorEnter = 0xc2,
    MonitorExit = 0xc3,
    MultiANewArray = 0xc5,
    New = 0xbb,
    NewArray = 0xbc,
    Nop = 0x0,
    Pop = 0x57,
    Pop2 = 0x58,
    PutField = 0xb5,
    PutStatic = 0xb3,
    Ret = 0xa9,
    Return = 0xb1,
    SALoad = 0x35,
    SAStore = 0x56,
    SIPush = 0x11,
    Swap = 0x5f,
    TableSwitch = 0xaa,
    Wide = 0xc4,
};

void codeGenBody(llvm::Function* function, const Code& code, const ClassFile& classFile, LazyClassLoaderHelper helper)
{
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(function->getContext(), "entry", function));
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

    std::vector<llvm::BasicBlock::iterator> insertPoints(code.getCode().size());

    std::vector<llvm::Value*> operandStack;
    operandStack.reserve(code.getMaxStack());

    llvm::ArrayRef<char> current = code.getCode();
    while (!current.empty())
    {
        auto offset = current.data() - code.getCode().data();

        if (!builder.GetInsertBlock()->empty())
        {
            insertPoints[offset] = builder.GetInsertBlock()->back().getIterator();
        }

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
            builder.SetInsertPoint(result->second);
        }

        auto addBasicBlock = [&](std::uint16_t target)
        {
            auto [result, inserted] = basicBlocks.insert({target, nullptr});
            if (!inserted)
            {
                return result->second;
            }

            if (target > offset)
            {
                return result->second = llvm::BasicBlock::Create(builder.getContext(), "", function);
            }

            auto iter = insertPoints[target];
            if (target == 0 && iter == function->getEntryBlock().end())
            {
                iter = function->getEntryBlock().begin();
            }
            else
            {
                iter = iter->getNextNode()->getIterator();
            }

            llvm::BasicBlock* original = iter->getParent();
            auto* newBlock = llvm::BasicBlock::Create(builder.getContext(), "", function, original);
            newBlock->splice(newBlock->end(), original, original->begin(), iter);
            llvm::IRBuilder<>::InsertPointGuard guard{builder};
            builder.SetInsertPoint(newBlock);
            builder.CreateBr(original);

            return result->second = original;
        };

        auto opCode = consume<OpCodes>(current);
        switch (opCode)
        {
            default: llvm_unreachable("NOT YET IMPLEMENTED");
            case OpCodes::AALoad:
            {
                llvm::Value* index = operandStack.back();
                operandStack.pop_back();
                llvm::Value* array = operandStack.back();
                operandStack.pop_back();

                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                              {builder.getInt32(0), builder.getInt32(2), index});
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), gep));
                break;
            }
            case OpCodes::AAStore:
            {
                llvm::Value* value = operandStack.back();
                operandStack.pop_back();
                llvm::Value* index = operandStack.back();
                operandStack.pop_back();
                llvm::Value* array = operandStack.back();
                operandStack.pop_back();

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
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[0]));
                break;
            case OpCodes::ALoad1:
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[1]));
                break;
            case OpCodes::ALoad2:
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[2]));
                break;
            case OpCodes::ALoad3:
                operandStack.push_back(builder.CreateLoad(referenceType(builder.getContext()), locals[3]));
                break;
            case OpCodes::ANewArray:
            {
                auto index = consume<PoolIndex<ClassInfo>>(current);
                llvm::Value* count = operandStack.back();
                operandStack.pop_back();

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
                llvm::Value* value = operandStack.back();
                operandStack.pop_back();
                builder.CreateRet(value);
                break;
            }
            case OpCodes::ArrayLength:
            {
                llvm::Value* array = operandStack.back();
                operandStack.pop_back();

                // The element type of the array type here is actually irrelevant.
                auto* gep = builder.CreateGEP(arrayStructType(referenceType(builder.getContext())), array,
                                              {builder.getInt32(0), builder.getInt32(1)});
                operandStack.push_back(builder.CreateLoad(builder.getInt32Ty(), gep));
                break;
            }
            case OpCodes::AStore:
            {
                auto index = consume<std::uint8_t>(current);
                builder.CreateStore(operandStack.back(), locals[index]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::AStore0:
            {
                builder.CreateStore(operandStack.back(), locals[0]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::AStore1:
            {
                builder.CreateStore(operandStack.back(), locals[1]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::AStore2:
            {
                builder.CreateStore(operandStack.back(), locals[2]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::AStore3:
            {
                builder.CreateStore(operandStack.back(), locals[3]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::BIPush:
            {
                auto byte = consume<std::int8_t>(current);
                llvm::Value* res = builder.getInt32(byte);
                operandStack.push_back(res);
                break;
            }
            case OpCodes::Dup:
            {
                operandStack.push_back(operandStack.back());
                break;
            }
            case OpCodes::GetField:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);
                const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(classFile);
                llvm::Type* type = descriptorToType(
                    parseFieldType(nameAndTypeInfo->descriptorIndex.resolve(classFile)->text), builder.getContext());

                llvm::Value* objectRef = operandStack.back();
                operandStack.pop_back();

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Value* fieldOffset = helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);

                llvm::Value* fieldPtr = builder.CreateGEP(builder.getInt8Ty(), objectRef, {fieldOffset});
                llvm::Value* field = builder.CreateLoad(type, fieldPtr);
                if (field->getType()->isIntegerTy() && field->getType()->getIntegerBitWidth() < 32)
                {
                    // Sign extend to the operands stack i32 type.
                    field = builder.CreateSExt(field, builder.getInt32Ty());
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
                llvm::Type* type = descriptorToType(parseFieldType(fieldType), builder.getContext());
                llvm::Value* field = builder.CreateLoad(type, fieldPtr);
                if (field->getType()->isIntegerTy() && field->getType()->getIntegerBitWidth() < 32)
                {
                    // Sign extend to the operands stack i32 type.
                    field = builder.CreateSExt(field, builder.getInt32Ty());
                }
                operandStack.push_back(field);
                break;
            }
            case OpCodes::Goto:
            {
                auto target = consume<std::int16_t>(current);
                builder.CreateBr(addBasicBlock(target + offset));
                break;
            }
            case OpCodes::New:
            {
                llvm::StringRef className =
                    consume<PoolIndex<ClassInfo>>(current).resolve(classFile)->nameIndex.resolve(classFile)->text;

                llvm::Value* classObject = helper.getClassObject(builder, "L" + className + ";");

                // Size is first 4 bytes in the class object and does not include the object header.
                llvm::Value* fieldAreaPtr = builder.CreateGEP(
                    builder.getInt8Ty(), classObject, {builder.getInt32(ClassObject::getFieldAreaSizeOffset())});
                llvm::Value* size = builder.CreateLoad(builder.getInt32Ty(), classObject);
                size = builder.CreateAdd(size, builder.getInt32(sizeof(ObjectHeader)));

                llvm::Module* module = function->getParent();
                llvm::Value* object = builder.CreateCall(allocationFunction(module), size);
                // Store object header (which in our case is just the class object) in the object.
                builder.CreateStore(classObject, object);
                operandStack.push_back(object);

                break;
            }
            case OpCodes::IAdd:
            {
                llvm::Value* lhs = operandStack.back();
                operandStack.pop_back();
                llvm::Value* rhs = operandStack.back();
                operandStack.pop_back();
                operandStack.push_back(builder.CreateAdd(lhs, rhs));
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
            case OpCodes::IfICmpGe:
            {
                auto target = consume<std::int16_t>(current);
                llvm::BasicBlock* basicBlock = addBasicBlock(target + offset);
                llvm::BasicBlock* next = addBasicBlock(current.data() - code.getCode().data());

                llvm::Value* rhs = operandStack.back();
                operandStack.pop_back();
                llvm::Value* lhs = operandStack.back();
                operandStack.pop_back();

                llvm::Value* cond = builder.CreateICmpSGE(lhs, rhs);
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
            case OpCodes::InvokeStatic:
            case OpCodes::InvokeSpecial:
            {
                const RefInfo* refInfo = consume<PoolIndex<RefInfo>>(current).resolve(classFile);

                bool isStatic = opCode == OpCodes::InvokeStatic;

                MethodType descriptor = parseMethodType(
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text);

                std::vector<llvm::Value*> args(descriptor.parameters.size() + (isStatic ? 0 : /*objectref*/ 1));
                for (auto& iter : llvm::reverse(args))
                {
                    iter = operandStack.back();
                    operandStack.pop_back();
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
                call->setAttributes(getABIAttributes(functionType));

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

                std::vector<llvm::Value*> args(descriptor.parameters.size() + 1);
                for (auto& iter : llvm::reverse(args))
                {
                    iter = operandStack.back();
                    operandStack.pop_back();
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
                call->setAttributes(getABIAttributes(functionType));

                if (descriptor.returnType != FieldType(BaseType::Void))
                {
                    operandStack.push_back(call);
                }
                break;
            }
            case OpCodes::IStore:
            {
                auto index = consume<std::uint8_t>(current);
                builder.CreateStore(operandStack.back(), locals[index]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::IStore0:
            {
                builder.CreateStore(operandStack.back(), locals[0]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::IStore1:
            {
                builder.CreateStore(operandStack.back(), locals[1]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::IStore2:
            {
                builder.CreateStore(operandStack.back(), locals[2]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::IStore3:
            {
                builder.CreateStore(operandStack.back(), locals[3]);
                operandStack.pop_back();
                break;
            }
            case OpCodes::ISub:
            {
                llvm::Value* lhs = operandStack.back();
                operandStack.pop_back();
                llvm::Value* rhs = operandStack.back();
                operandStack.pop_back();
                operandStack.push_back(builder.CreateSub(lhs, rhs));
                break;
            }
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
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });

                break;
            }
            case OpCodes::PutField:
            {
                const auto* refInfo = consume<PoolIndex<FieldRefInfo>>(current).resolve(classFile);

                llvm::Value* value = operandStack.back();
                operandStack.pop_back();
                llvm::Value* objectRef = operandStack.back();
                operandStack.pop_back();

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;
                llvm::Value* fieldOffset = helper.getInstanceFieldOffset(builder, className, fieldName, fieldType);

                llvm::Value* fieldPtr =
                    builder.CreateGEP(llvm::Type::getInt8Ty(builder.getContext()), objectRef, {fieldOffset});

                llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
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
                llvm::Value* value = operandStack.back();
                operandStack.pop_back();

                llvm::StringRef className = refInfo->classIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldName =
                    refInfo->nameAndTypeIndex.resolve(classFile)->nameIndex.resolve(classFile)->text;
                llvm::StringRef fieldType =
                    refInfo->nameAndTypeIndex.resolve(classFile)->descriptorIndex.resolve(classFile)->text;

                llvm::Value* fieldPtr = helper.getStaticFieldAddress(builder, className, fieldName, fieldType);

                llvm::Type* llvmFieldType = descriptorToType(parseFieldType(fieldType), builder.getContext());
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
            case OpCodes::Pop:
            {
                operandStack.pop_back();
                break;
            }
            case OpCodes::Return: builder.CreateRetVoid(); break;
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
                                      m_baseLayer, m_interner, m_dataLayout));

    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}
