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

#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/IR/IRBuilder.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/debuginfo/TrivialDebugInfoBuilder.hpp>
#include <jllvm/object/Object.hpp>

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
};

/// Specialization for float.
template <>
struct CppToLLVMType<float>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::Type::getFloatTy(*context);
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
};

/// Specialization for void.
template <>
struct CppToLLVMType<void>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::Type::getVoidTy(*context);
    }
};

/// Specialization for any pointer type.
template <class T>
requires(!std::is_base_of_v<ObjectInterface, T>) struct CppToLLVMType<T*>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return llvm::PointerType::get(*context, 0);
    }
};

/// Specialization for Objects type.
template <std::derived_from<ObjectInterface> T>
struct CppToLLVMType<T*>
{
    static llvm::Type* get(llvm::LLVMContext* context)
    {
        return referenceType(*context);
    }
};

template <class F>
class LambdaMaterializationUnit : public llvm::orc::MaterializationUnit
{
    std::string m_symbol;
    llvm::orc::IRLayer& m_baseLayer;
    F m_f;
    llvm::DataLayout m_dataLayout;

    // Technically speaking, the property we require is trivially copyable.
    // However,GCC is stricter when it comes to determining trivially copyable of lambdas, making asserts fail when
    // an equivalent struct with operator() wouldn't.
    // Admittedly, GCC is allowed to do that since it is apparently implementation defined whether lambdas are
    // trivially copyable.
    // We therefore instead use trivially destructible as a check instead to still catch issues such as a lambda having
    // non-trivial types within. Given the prevalence of the Rule of 5 this should be just as effective at catching
    // these errors.
    static_assert(std::is_trivially_destructible_v<F>);

    template <std::size_t... is>
    std::array<llvm::Type*, sizeof...(is)> parameterTypes(std::index_sequence<is...>, llvm::LLVMContext* context)
    {
        return {CppToLLVMType<typename llvm::function_traits<F>::template arg_t<is>>::get(context)...};
    }

    template <class Ret, class... Args>
    static Ret trampolineFunction(Args... args, F* f) noexcept(false)
    {
        return (*f)(args...);
    }

    template <std::size_t... is>
    auto trampoline(std::index_sequence<is...>)
    {
        return reinterpret_cast<std::uintptr_t>(
            trampolineFunction<typename llvm::function_traits<F>::result_t,
                               typename llvm::function_traits<F>::template arg_t<is>...>);
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
        function->addFnAttr(llvm::Attribute::getWithUWTableKind(function->getContext(), llvm::UWTableKind::Async));

        TrivialDebugInfoBuilder trivialDebugInfo(function);

        auto* type = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sizeof(F));
        auto* closure = new llvm::GlobalVariable(
            *module, type, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantDataArray::getRaw(llvm::StringRef(reinterpret_cast<char*>(&m_f), sizeof(F)), sizeof(F),
                                            llvm::Type::getInt8Ty(*context)),
            "closure");
        closure->setAlignment(llvm::Align(alignof(F)));

        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

        builder.SetCurrentDebugLocation(trivialDebugInfo.getNoopLoc());

        llvm::SmallVector<llvm::Value*> args;
        for (auto& iter : function->args())
        {
            args.push_back(&iter);
        }
        args.push_back(closure);
        auto callee = builder.CreateIntToPtr(
            builder.getInt64(trampoline(std::make_index_sequence<llvm::function_traits<F>::num_args>{})),
            builder.getPtrTy());

        auto types = llvm::to_vector(parameters);
        types.push_back(builder.getPtrTy());
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

        trivialDebugInfo.finalize();

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
std::unique_ptr<llvm::orc::MaterializationUnit>
    createLambdaMaterializationUnit(std::string symbol, llvm::orc::IRLayer& baseLayer, const F& f,
                                    const llvm::DataLayout& dataLayout, llvm::orc::MangleAndInterner& interner)
{
    auto functionPointerType = []<std::size_t... is>(std::index_sequence<is...>) ->
        typename llvm::function_traits<F>::result_t (*)(typename llvm::function_traits<F>::template arg_t<is>...)
    { return nullptr; };
    using FnType = decltype(functionPointerType(std::make_index_sequence<llvm::function_traits<F>::num_args>{}));

    // Optimize trivial capture-less lambdas by converting them to function pointers and defining them as absolute
    // symbols instead. This avoids creating the lambda object stub leading to better code being generated.
    if constexpr (std::is_convertible_v<F, FnType>)
    {
        return llvm::orc::absoluteSymbols(
            {{interner(symbol),
              llvm::JITEvaluatedSymbol::fromPointer(static_cast<FnType>(f),
                                                    llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable)}});
    }
    else
    {
        return std::make_unique<LambdaMaterializationUnit<F>>(std::move(symbol), baseLayer, f, dataLayout, interner);
    }
}

} // namespace jllvm
