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

#include <llvm/ADT/STLExtras.h>

#include <jllvm/gc/RootFreeList.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <cstdint>
#include <utility>

#include <jni.h>

#include "VirtualMachine.hpp"

namespace jllvm
{

class VirtualMachine;

/// Returns the 'VirtualMachine' instance associated with the 'JNIEnv'.
VirtualMachine& virtualMachineFromJNIEnv(JNIEnv* env);

/// Struct specialised to perform translation between JNI types and JLLVM types.
/// 'T' is either the JNI or JLLVM type, with the type it converts to being the type returned by the call operator.
///
/// Default behaviour just passes through the values.
template <class T>
struct JNIConvert
{
    /// Call operator that performs the conversion between JNI types and JLLVM types.
    T operator()(VirtualMachine&, T value)
    {
        return value;
    }
};

/// The type 'T' converts to.
template <class T>
using JNIConverted = decltype(JNIConvert<T>{}(std::declval<VirtualMachine&>(), std::declval<T>()));

/// Concept satisfied if the JNI conversion of 'T' is symmetric, i.e. the type 'T' converts to, also converts back to
/// 'T'.
template <class T>
concept JNIConversionSymmetric = std::is_same_v<T, JNIConverted<JNIConverted<T>>>;

/// Base class useful to define JNI conversions that are just a bit cast from 'From' to 'To'.
template <class From, class To>
struct JNIBitCastConvert
{
    To operator()(VirtualMachine&, From value)
    {
        static_assert(sizeof(From) == sizeof(To));
        static_assert(std::is_trivially_copyable_v<From>);

        To to;
        std::memcpy(&to, &value, sizeof(value));
        return to;
    }
};

/// Conversion of pointer to Java objects to JNI. Performs rooting before then using the conversion of 'GCRootRef'.
template <std::derived_from<ObjectInterface> T>
struct JNIConvert<T*>
{
    JNIConverted<GCRootRef<T>> operator()(VirtualMachine& virtualMachine, T* value)
    {
        if (!value)
        {
            // Null values must be null in JNI as well.
            return nullptr;
        }
        return JNIConvert<GCRootRef<T>>{}(virtualMachine, virtualMachine.getGC().root(value).release());
    }
};

#define BITCAST(From, To)                                 \
    template <>                                           \
    struct JNIConvert<From> : JNIBitCastConvert<From, To> \
    {                                                     \
    };                                                    \
                                                          \
    template <>                                           \
    struct JNIConvert<To> : JNIBitCastConvert<To, From>   \
    {                                                     \
    }

BITCAST(GCRootRef<ClassObject>, jclass);
BITCAST(GCRootRef<ObjectInterface>, jobject);
BITCAST(GCRootRef<AbstractArray>, jarray);
BITCAST(GCRootRef<Array<ObjectInterface*>>, jobjectArray);
BITCAST(GCRootRef<Array<jboolean>>, jbooleanArray);
BITCAST(GCRootRef<Array<jbyte>>, jbyteArray);
BITCAST(GCRootRef<Array<jchar>>, jcharArray);
BITCAST(GCRootRef<Array<jshort>>, jshortArray);
BITCAST(GCRootRef<Array<jint>>, jintArray);
BITCAST(GCRootRef<Array<jlong>>, jlongArray);
BITCAST(GCRootRef<Array<jfloat>>, jfloatArray);
BITCAST(GCRootRef<Array<jdouble>>, jdoubleArray);
BITCAST(Field*, jfieldID);

#undef BITCAST

/// Allow returning 'const Field*' as well.
template <>
struct JNIConvert<const Field*>
{
    decltype(auto) operator()(VirtualMachine& virtualMachine, const Field* value)
    {
        return JNIConvert<Field*>{}(virtualMachine, const_cast<Field*>(value));
    }
};

/// Converts a capture-less lambda using JLLVM types in its signature to a function pointer with corresponding JNI
/// types. The lambda is required to take 'VirtualMachine&' as its first parameter. The function pointer returned is
/// then of type '<ret-converted>(*)(JNIEnv*, <params-converted>)'
///
/// The conversion is performed using specializations of 'JNIConvert'. The parameter types are required to be types
/// that have a symmetric conversion to JNI types. In other words, the 'JNIConvert' specialization from the lambda
/// parameter types are used to create the JNI function parameters. The 'JNIConvert' from the JNI function parameter
/// types is then used to convert the JNI types to the JLLVM types.
///
/// The return value is converted using 'JNIConvert'. There is no symmetry restriction for the return type.
template <class Lambda>
auto translateJNIInterface(Lambda) requires std::is_empty_v<Lambda>&& std::is_default_constructible_v<Lambda>
{
    // Initial 0 to discard the implicit 'VirtualMachine&' parameter of 'Lambda'.
    return +[]<std::size_t... idx>(std::index_sequence<0, idx...>)
    {
        static_assert((JNIConversionSymmetric<typename llvm::function_traits<Lambda>::template arg_t<idx>> && ...)
                      && "Parameter types must have a symmetric conversion");

        return [](JNIEnv* env, JNIConverted<typename llvm::function_traits<Lambda>::template arg_t<idx>>... args)
        {
            VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
            if constexpr (std::is_void_v<typename llvm::function_traits<Lambda>::result_t>)
            {
                Lambda{}(virtualMachine,
                         JNIConvert<JNIConverted<typename llvm::function_traits<Lambda>::template arg_t<idx>>>{}(
                             virtualMachine, args)...);
            }
            else
            {
                return JNIConvert<typename llvm::function_traits<Lambda>::result_t>{}(
                    virtualMachine,
                    Lambda{}(virtualMachine,
                             JNIConvert<JNIConverted<typename llvm::function_traits<Lambda>::template arg_t<idx>>>{}(
                                 virtualMachine, args)...));
            }
        };
    }(std::make_index_sequence<llvm::function_traits<Lambda>::num_args>());
}

} // namespace jllvm
