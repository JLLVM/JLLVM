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

#include <llvm/ADT/StringRef.h>

#include <jllvm/support/FileUtils.hpp>

#include <string_view>

#include "JNIImplementation.hpp"
#include "VirtualMachine.hpp"

namespace jllvm
{

/// Base class for any Models used as our high level API for implementing native methods of Java.
/// This high level API builds on top of the JNI and translates the JNIs general and JVM agnostic C interface to
/// a more high level C++ API specific to our JVM implementation.
///
/// To implement a new implementation of native methods for a Java class, simply create a new model class inheriting
/// from 'ModelBase'.
///
/// 'StateType' refers to a subclass of 'ModelState', of which a per VM singleton will be default constructed
/// when registering the model and injected into static methods when requested
/// or can be accessed from non-static methods using the 'state' field.
//
/// Use case for this type is the ability to persist state between function calls within a Model. This is commonly
/// used to create and then use 'InstanceFieldRef' or 'StaticFieldRef's without having to look them up on every call.
///
/// The second optional template parameter describes the type that should be used as object representation for any 'this'
/// object coming from Java. By default it is simply 'Object'.
///
/// The model subclass may then simply implement member functions with the EXACT same name as the 'native' method in
/// Java. Function parameters from Java can easily be translated to C++ types: For integer types simply use signed
/// integer types with the same width as the corresponding Java type. For Java Objects, use 'GCRootRef<Object>' or any
/// other subclass of 'ObjectInterface' (such as e.g. 'GCRootRef<Array<int>>') that corresponds to, or is at least a
/// super class, of the object used in the Java API.
/// For convenience, the return type may be either 'GCRootRef<T>' or 'T*' when returning Java objects.
///
/// Non-static method members do NOT need an explicit 'this' pointer parameter. You can use 'javaThis' from within
/// member functions to get the 'this' reference from Java.
///
/// Static methods generally have 'GCRootRef<ClassObject>' as their first parameter.
/// The parameters from the static method from Java follow right afterwards. The 'GCRootRef<ClassObject>' is the class
/// object of the class the static method is defined in.
/// Optionally, static methods may have additional parameters prior to the class object to inject additional state
/// required for the implementation. These may be one of:
/// * 'VirtualMachine&'
/// * 'State&, VirtualMachine&'
/// * 'State&'
///
/// As a final step, 'ModelBase' subclasses must add the following 'constexpr static' fields:
/// * 'llvm::StringLiteral className' which should contain the fully qualified name (i.e. with slashes) of the class
///    being modelled
/// *  'auto methods = std::make_tuple(&ModelClass::aNativeMethod, ...)' which is a tuple that should list ALL
///    implementations of 'native' methods that should be registered in the VM.
template <class StateType = ModelState, class JavaObject = Object>
class ModelBase
{
    static_assert(std::is_base_of_v<ModelState, StateType>, "State must inherit from ModelState");

    static_assert(
        std::is_base_of_v<ObjectInterface, JavaObject>,
        "JavaObject must be a valid Java object representation with an object header and inherits from ObjectInterface");

protected:
    using Base = ModelBase<StateType, JavaObject>;

    /// 'this' object from Java to be used in member functions.
    GCRootRef<JavaObject> javaThis;
    /// Instance of the virtual machine this model is registered in.
    VirtualMachine& virtualMachine;
    /// State instance of this model
    StateType& state;

public:
    /// Constructor required to be implemented by any subclasses. Simply adding 'using Base::Base' to subclasses will
    /// make them inherit this constructor.
    ModelBase(GCRootRef<JavaObject> javaThis, VirtualMachine& virtualMachine, StateType& state)
        : javaThis(javaThis), virtualMachine(virtualMachine), state(state)
    {
    }

    /// Object representation type of Javas 'this'.
    using ThisType = JavaObject;
    /// 'State' type of a model.
    using State = StateType;
};

/// Register any models for builtin Java classes in the VM.
void registerJavaClasses(VirtualMachine& virtualMachine);

namespace detail
{

auto coerceReturnType(std::derived_from<ObjectInterface> auto* object, VirtualMachine& virtualMachine)
{
    // JNI only accepts roots as parameters and return type. Coerce any pointer to objects returned to roots.
    // The JNI bridge will delete the root and pass the object back to the Java stack.
    return virtualMachine.getGC().root(object).release();
}

template <class T>
auto coerceReturnType(T object, VirtualMachine&) requires(std::is_arithmetic_v<T>)
{
    return object;
}

template <class T>
auto coerceReturnType(GCRootRef<T> ref, VirtualMachine&)
{
    return ref;
}

// Static 'VirtualMachine&, Args...' method.
template <class Model, class Ret, class... Args>
auto createMethodBridge(typename Model::State&, Ret (*ptr)(VirtualMachine&, GCRootRef<ClassObject>, Args...))
{
    return [ptr](JNIEnv* env, GCRootRef<ClassObject> classObject, Args... args)
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        if constexpr (!std::is_void_v<Ret>)
        {
            return coerceReturnType(ptr(virtualMachine, classObject, args...), virtualMachine);
        }
        else
        {
            return ptr(virtualMachine, classObject, args...);
        }
    };
}

// Static 'State&, Args...' method.
template <class Model, class Ret, class... Args>
auto createMethodBridge(typename Model::State& state,
                        Ret (*ptr)(typename Model::State&, GCRootRef<ClassObject>, Args...))
{
    return [ptr, &state](JNIEnv* env, GCRootRef<ClassObject> classObject, Args... args)
    {
        if constexpr (!std::is_void_v<Ret>)
        {
            VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
            return coerceReturnType(ptr(state, classObject, args...), virtualMachine);
        }
        else
        {
            return ptr(state, classObject, args...);
        }
    };
}

// Static 'State&, VirtualMachine&, Args...' method.
template <class Model, class Ret, class... Args>
auto createMethodBridge(typename Model::State& state,
                        Ret (*ptr)(typename Model::State&, VirtualMachine&, GCRootRef<ClassObject>, Args...))
{
    return [ptr, &state](JNIEnv* env, GCRootRef<ClassObject> classObject, Args... args)
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        if constexpr (!std::is_void_v<Ret>)
        {
            return coerceReturnType(ptr(state, virtualMachine, classObject, args...), virtualMachine);
        }
        else
        {
            return ptr(state, virtualMachine, classObject, args...);
        }
    };
}

// Static 'Args...' method.
template <class Model, class Ret, class... Args>
auto createMethodBridge(typename Model::State&, Ret (*ptr)(GCRootRef<ClassObject>, Args...))
{
    return [ptr](JNIEnv* env, GCRootRef<ClassObject> classObject, Args... args)
    {
        if constexpr (!std::is_void_v<Ret>)
        {
            VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
            return coerceReturnType(ptr(classObject, args...), virtualMachine);
        }
        else
        {
            return ptr(classObject, args...);
        }
    };
}

// Instance method.
template <class Model, class Ret, class... Args>
auto createMethodBridge(typename Model::State& state, Ret (Model::*ptr)(Args...))
{
    return [ptr, &state](JNIEnv* env, GCRootRef<typename Model::ThisType> javaThis, Args... args)
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        if constexpr (!std::is_void_v<Ret>)
        {
            return coerceReturnType((Model(javaThis, virtualMachine, state).*ptr)(args...),
                                    virtualMachine);
        }
        else
        {
            return (Model(javaThis, virtualMachine, state).*ptr)(args...);
        }
    };
}

// WARNING: fnPtr even if unused is required to be named for clang to include it in the __PRETTY_FUNCTION__ output
// below. Massive hack, I know.
template <auto fnPtr>
constexpr auto functionName()
{
    // GCC and Clang encode the typename here by appending at the back something similar to [fnPtr = Class::name]
    std::string_view demangledName = __PRETTY_FUNCTION__;
    std::size_t rstart = demangledName.rfind(']');
    assert(rstart != std::string_view::npos);
    // Skip over possible whitespace.
    while (rstart > 0 && demangledName[rstart - 1] == ' ')
    {
        rstart--;
    }

    // Last namespace qualifier from the back.
    constexpr std::string_view namespaceQual = "::";
    std::size_t lastNamespaceQual = demangledName.rfind(namespaceQual, rstart);
    assert(lastNamespaceQual != std::string_view::npos);

    demangledName = demangledName.substr(lastNamespaceQual + namespaceQual.size(),
                                         rstart - (lastNamespaceQual + namespaceQual.size()));
    return demangledName;
}

template <class T>
T& emptyInstance()
{
    static T result;
    return result;
}

} // namespace detail

/// Registers all methods of a model 'Model' within 'virtualMachine'.
template <class Model>
void addModel(VirtualMachine& virtualMachine)
{
    static_assert(
        requires {
            {
                Model::className
            } -> std::convertible_to<llvm::StringRef>;
        },
        "'Model' must have a 'constexpr static llvm::StringLiteral className' field with fully qualified name of the class it is modelling");

    static_assert(
        requires {
            Model::methods;
            std::tuple_size_v<decltype(Model::methods)>;
            std::get<0>(Model::methods);
        },
        "'Model' must have a 'constexpr static' tuple called 'methods' listing all 'native' methods implemented");

    constexpr auto methods = Model::methods;
    using State = typename Model::State;
    auto& state = [&]() -> State&
    {
        if constexpr (std::is_empty_v<State>)
        {
            // Don't waste a memory allocation for any empty state.
            // Just create a global instead.
            return detail::emptyInstance<State>();
        }
        else
        {
            return virtualMachine.allocModelState<State>();
        }
    }();

    [&]<std::size_t... idxs>(std::index_sequence<idxs...>)
    {
        (
            [&]
            {
                constexpr auto fn = std::get<idxs>(methods);
                constexpr std::string_view methodName = detail::functionName<fn>();
                virtualMachine.getJIT().addJNISymbol(formJNIMethodName(Model::className, methodName),
                                                     detail::createMethodBridge<Model>(state, fn));
            }(),
            ...);
    }(std::make_index_sequence<std::tuple_size_v<decltype(methods)>>{});
}

template <class... Models>
void addModels(jllvm::VirtualMachine& virtualMachine)
{
    (addModel<Models>(virtualMachine), ...);
}

} // namespace jllvm
