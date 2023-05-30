#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/IR/IRBuilder.h>

namespace jllvm
{
/// Struct that should be specialized to provide a mapping between C++ and LLVM types and constants.
template <class T>
struct CppToLLVMType;

/// Specialization for any integer type.
template <class T>
requires(std::is_integral_v<T>) struct CppToLLVMType<T>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::IntegerType::getIntNTy(*context, sizeof(T) * CHAR_BIT);
    }

    static llvm::Value* getConstant(T value, llvm::IRBuilder<>& builder)
    {
        return builder.getIntN(sizeof(T) * CHAR_BIT, value);
    }
};

/// Specialization for float.
template <>
struct CppToLLVMType<float>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::Type::getFloatTy(*context);
    }

    static llvm::Value* getConstant(float value, llvm::IRBuilder<>& builder)
    {
        return llvm::ConstantFP::get(builder.getFloatTy(), value);
    }
};

/// Specialization for double.
template <>
struct CppToLLVMType<double>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::Type::getDoubleTy(*context);
    }

    static llvm::Value* getConstant(double value, llvm::IRBuilder<>& builder)
    {
        return llvm::ConstantFP::get(builder.getDoubleTy(), value);
    }
};

/// Specialization for void.
template <>
struct CppToLLVMType<void>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::Type::getVoidTy(*context);
    }

    template <class T>
    static llvm::Value* getConstant(T, llvm::IRBuilder<>&)
    {
        return nullptr;
    }
};

/// Specialization for any pointer type.
template <class T>
struct CppToLLVMType<T*>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::PointerType::get(*context, 0);
    }

    static llvm::Value* getConstant(T* value, llvm::IRBuilder<>& builder)
    {
        return builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(value)),
                                      get(&builder.getContext()));
    }
};

template <class F>
class LambdaMaterializationUnit : public llvm::orc::MaterializationUnit
{
    std::string m_symbol;
    llvm::orc::IRLayer& m_baseLayer;
    F m_f;
    llvm::DataLayout m_dataLayout;

    // GCC is stricter when it comes to determining trivially copyable of lambdas.
    // Admittedly, GCC is allowed to do that since it is apparently implementation defined whether lambdas are
    // trivially copyable.
    // Practically speaking this should always work for now and in the future it is possible to drop the trivially
    // copyable requirements (which only exists because we effectively memcpy the lambda) if ever required.
#ifdef __clang__
    static_assert(std::is_trivially_copyable_v<F>);
#endif

    template <std::size_t... is>
    std::array<llvm::Type*, sizeof...(is)> parameterTypes(std::index_sequence<is...>, llvm::LLVMContext* context)
    {
        return {CppToLLVMType<typename llvm::function_traits<F>::template arg_t<is>>::get(context)...};
    }

    template <std::size_t... is>
    auto trampoline(std::index_sequence<is...>)
    {
        return reinterpret_cast<std::uintptr_t>(+[](F* f, typename llvm::function_traits<F>::template arg_t<is>... args)
                                                { return (*f)(args...); });
    }

public:
    LambdaMaterializationUnit(std::string&& symbol, llvm::orc::IRLayer& baseLayer, const F& f,
                              const llvm::DataLayout& dataLayout, llvm::orc::MangleAndInterner& interner)
        : llvm::orc::MaterializationUnit(
            [&]
            {
                llvm::orc::SymbolFlagsMap result;
                result[interner(symbol)] = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
                return llvm::orc::MaterializationUnit::Interface(std::move(result), nullptr);
            }()),
          m_symbol(std::move(symbol)),
          m_baseLayer(baseLayer),
          m_f(f),
          m_dataLayout(dataLayout)
    {
    }

    llvm::StringRef getName() const override
    {
        return "LambdaMaterializationUnit";
    }

    void materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr) override
    {
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>(getName(), *context);
        module->setDataLayout(m_dataLayout);
        module->setTargetTriple(LLVM_HOST_TRIPLE);

        llvm::Type* retType = CppToLLVMType<typename llvm::function_traits<F>::result_t>::get(context.get());
        auto parameters = parameterTypes(std::make_index_sequence<llvm::function_traits<F>::num_args>{}, context.get());
        auto functionType = llvm::FunctionType::get(retType, parameters, false);

        llvm::Function* function =
            llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage, m_symbol, module.get());

        auto* type = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sizeof(F));
        auto* closure = new llvm::GlobalVariable(
            *module, type, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantDataArray::getRaw(llvm::StringRef(reinterpret_cast<char*>(&m_f), sizeof(F)), sizeof(F),
                                            llvm::Type::getInt8Ty(*context)),
            "closure");
        closure->setAlignment(llvm::Align(alignof(F)));

        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

        llvm::SmallVector<llvm::Value*> args;
        args.push_back(closure);
        for (auto& iter : function->args())
        {
            args.push_back(&iter);
        }
        auto callee = builder.CreateIntToPtr(
            builder.getInt64(trampoline(std::make_index_sequence<llvm::function_traits<F>::num_args>{})),
            builder.getPtrTy());

        auto types = llvm::to_vector(parameters);
        types.insert(types.begin(), builder.getPtrTy());
        auto* trampFuncType = llvm::FunctionType::get(retType, types, false);

        auto* call = builder.CreateCall(trampFuncType, callee, args);
        if (retType->isVoidTy())
        {
            builder.CreateRetVoid();
        }
        else
        {
            builder.CreateRet(call);
        }

        m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
    }

private:
    void discard(const llvm::orc::JITDylib&, const llvm::orc::SymbolStringPtr&) override
    {
        llvm_unreachable("unreachable");
    }
};

/// Convenience materialization unit allowing the lambda 'f' with state to be bound to 'symbol'. The advantage
/// of this is that it also works with stateful lambdas as long as they are trivially copyable. This works
/// by JIT compiling a trampoline method where the lambda is copied into and then calling the lambdas actual call
/// operator using it as the first argument.
/// Note that this requires a known mapping between the argument types in C++ and LLVM. You can provide these via
/// specializing 'CppToLLVMType'.
template <class F>
std::unique_ptr<LambdaMaterializationUnit<F>>
    createLambdaMaterializationUnit(std::string symbol, llvm::orc::IRLayer& baseLayer, const F& f,
                                    const llvm::DataLayout& dataLayout, llvm::orc::MangleAndInterner& interner)
{
    return std::make_unique<LambdaMaterializationUnit<F>>(std::move(symbol), baseLayer, f, dataLayout, interner);
}

} // namespace jllvm
