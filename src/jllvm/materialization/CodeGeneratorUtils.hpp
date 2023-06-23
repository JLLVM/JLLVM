#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <jllvm/class/ByteCodeIterator.hpp>
#include <jllvm/object/ClassLoader.hpp>

#include "ByteCodeCompileUtils.hpp"

namespace jllvm
{

/// Class for Java bytecode typechecking
/// This works by iterating over the bytecode of a Java method extracting its basic block
/// and the types on the stack at the start of the block.
/// It constructs the map of basic blocks from their starting offset inside the bytecode to the starting state of their
/// stack.
class ByteCodeTypeChecker
{
    struct ReturnInfo
    {
        std::uint16_t retOffset;
        std::uint16_t returnAddress;
    };

public:
    using RetAddrType = llvm::PointerEmbeddedInt<std::uint16_t>;
    using JVMType = llvm::PointerUnion<llvm::Type*, RetAddrType>;
    using TypeStack = std::vector<JVMType>;
    using BasicBlockMap = llvm::DenseMap<std::uint16_t, TypeStack>;
    using PossibleRetsMap = llvm::DenseMap<std::uint16_t, llvm::DenseSet<std::uint16_t>>;

private:
    llvm::LLVMContext& m_context;
    const ClassFile& m_classFile;
    const Code& m_code;
    std::vector<std::uint16_t> m_offsetStack;
    llvm::DenseMap<std::uint16_t, std::uint16_t> m_localRetMap;
    llvm::DenseMap<std::uint16_t, std::uint16_t> m_returnAddressToSubroutineMap;
    llvm::DenseMap<std::uint16_t, ReturnInfo> m_subroutineToReturnInfoMap;
    BasicBlockMap m_basicBlocks;
    llvm::Type* m_addressType;
    llvm::Type* m_doubleType;
    llvm::Type* m_floatType;
    llvm::Type* m_intType;
    llvm::Type* m_longType;

    void checkBasicBlock(llvm::ArrayRef<char> block, std::uint16_t offset, TypeStack typeStack);

    void check();

public:
    ByteCodeTypeChecker(llvm::LLVMContext& context, const ClassFile& classFile, const Code& code)
        : m_context{context},
          m_classFile{classFile},
          m_code{code},
          m_addressType{referenceType(m_context)},
          m_doubleType{llvm::Type::getDoubleTy(m_context)},
          m_floatType{llvm::Type::getFloatTy(m_context)},
          m_intType{llvm::Type::getInt32Ty(m_context)},
          m_longType{llvm::Type::getInt64Ty(m_context)}
    {
        check();
    }

    /// Creates a mapping between each 'ret' instruction and the offset inside the bytecode where it could return to.
    PossibleRetsMap makeRetToMap();

    const BasicBlockMap& getBasicBlocks() const
    {
        return m_basicBlocks;
    }
};

/// Class for JVM operand stack
/// This class also offers method to save and restore the current state of the stack in order to consider the control
/// flow path
class OperandStack
{
    std::vector<llvm::AllocaInst*> m_values;
    std::vector<llvm::Type*> m_types;
    llvm::IRBuilder<>& m_builder;
    std::size_t m_topOfStack{};

public:
    using State = std::vector<llvm::Type*>;

    OperandStack(llvm::IRBuilder<>& builder, std::uint16_t maxStack)
        : m_builder(builder), m_values{maxStack}, m_types{maxStack}
    {
        std::generate(m_values.begin(), m_values.end(), [&] { return builder.CreateAlloca(builder.getPtrTy()); });
    }

    llvm::Value* pop_back()
    {
        llvm::AllocaInst* alloc = m_values[--m_topOfStack];
        llvm::Type* type = m_types[m_topOfStack];
        return m_builder.CreateLoad(type, alloc);
    }

    std::pair<llvm::Value*, llvm::Type*> pop_back_with_type()
    {
        llvm::AllocaInst* alloc = m_values[--m_topOfStack];
        llvm::Type* type = m_types[m_topOfStack];

        return {m_builder.CreateLoad(type, alloc), type};
    }

    void push_back(llvm::Value* value)
    {
        llvm::AllocaInst* alloc = m_values[m_topOfStack];
        m_types[m_topOfStack++] = value->getType();
        m_builder.CreateStore(value, alloc);
    }

    void setState(const State& state)
    {
        llvm::copy(state, m_types.begin());
        m_topOfStack = state.size();
    }

    void setHandlerStack(llvm::Value* value)
    {
        llvm::AllocaInst* alloc = m_values.front();
        m_types.front() = value->getType();
        m_builder.CreateStore(value, alloc);
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

    static void buildClassInitializerInitStub(llvm::IRBuilder<>& builder, const ClassObject& classObject);

    template <class F>
    llvm::Value* returnConstantForClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor, llvm::Twine key,
                                              F&& f, bool mustInitializeClassObject);

    template <class F>
    llvm::Value* doCallForClassObject(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef methodName,
                                      llvm::StringRef methodType, bool isStatic, llvm::Twine key,
                                      llvm::ArrayRef<llvm::Value*> args, F&& f);

    struct VTableOffset
    {
        std::size_t slot;
    };

    struct ITableOffset
    {
        std::size_t interfaceId;
        std::size_t slot;
    };

    using ResolutionResult = swl::variant<VTableOffset, ITableOffset, std::string>;

    static ResolutionResult vTableResult(const ClassObject* classObject, const Method* method);

    static ResolutionResult iTableResult(const ClassObject* interface, const Method* method);

    static ResolutionResult virtualMethodResolution(const ClassObject* classObject, llvm::StringRef methodName,
                                                    llvm::StringRef methodType);

    static ResolutionResult interfaceMethodResolution(const ClassObject* classObject, llvm::StringRef methodName,
                                                      llvm::StringRef methodType, ClassLoader& classLoader);

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
        m_mainDylib.withLinkOrderDo([&](const llvm::orc::JITDylibSearchOrder& dylibSearchOrder)
                                    { m_implDylib.setLinkOrder(dylibSearchOrder); });
    }

    /// Creates a non-virtual call to the possibly static function 'methodName' of the type 'methodType' within
    /// 'className' using 'args'. This is used to implement `invokestatic` and `invokespecial`.
    llvm::Value* doNonVirtualCall(llvm::IRBuilder<>& builder, bool isStatic, llvm::StringRef className,
                                  llvm::StringRef methodName, llvm::StringRef methodType,
                                  llvm::ArrayRef<llvm::Value*> args);

    enum MethodResolution
    {
        /// 5.4.3.3. Method Resolution from the JVM Spec.
        Virtual,
        /// 5.4.3.4. Interface Method Resolution from the JVM Spec.
        Interface
    };

    /// Creates a virtual call to the function 'methodName' of the type 'methodType' within 'className' using 'args'.
    /// 'resolution' determines how the actual method to be called is resolved using the previously mentioned strings.
    llvm::Value* doIndirectCall(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef methodName,
                                llvm::StringRef methodType, llvm::ArrayRef<llvm::Value*> args,
                                MethodResolution resolution);

    /// Returns an LLVM integer constant which contains the offset of the 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                        llvm::StringRef fieldName, llvm::StringRef fieldType);

    /// Returns an LLVM Pointer which points to the static field 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef fieldName,
                                       llvm::StringRef fieldType);

    /// Returns an LLVM Pointer which points to the class object of the type with the given field descriptor.
    llvm::Value* getClassObject(llvm::IRBuilder<>& builder, llvm::Twine fieldDescriptor,
                                bool mustInitializeClassObject = false);
};
} // namespace jllvm
