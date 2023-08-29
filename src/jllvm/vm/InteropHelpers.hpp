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

#include <llvm/ExecutionEngine/JITSymbol.h>

#include <jllvm/gc/RootFreeList.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <concepts>
#include <type_traits>

namespace jllvm
{

namespace detail
{
// JavaCompatible types convert to themselves.
template <JavaCompatible T>
T javaConvertedType(T);

// GCRootRefs convert to their contained object.
template <class T>
T* javaConvertedType(GCRootRef<T>);

} // namespace detail

/// Type alias returning the 'JavaCompatible' type a 'JavaConvertible' type implicitly converts to.
template <class T>
using JavaConvertedType = decltype(detail::javaConvertedType(std::declval<T>()));

/// Concept for any type that is known to implicitly convert to a 'JavaCompatible' type.
template <class T>
concept JavaConvertible = !std::is_void_v<T> && JavaCompatible<JavaConvertedType<T>>;

/// Helper function to call 'fnPtr', which is known to be a Java function, with the given 'args'.
/// Does implicit conversion of 'args' their 'JavaCompatible' type.
template <JavaCompatible Ret, JavaConvertible... Args>
Ret invokeJava(llvm::JITEvaluatedSymbol fnPtr, Args... args)
{
    return reinterpret_cast<Ret (*)(JavaConvertedType<Args>...)>(fnPtr.getAddress())(args...);
}

} // namespace jllvm
