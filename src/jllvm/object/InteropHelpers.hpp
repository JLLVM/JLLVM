// Copyright (C) 2024 The JLLVM Contributors.
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

#include <concepts>
#include <type_traits>

namespace jllvm
{

class ObjectInterface;

/// Concept for any Java object aka subtypes of 'ObjectInterface'.
template <class T>
concept JavaObject = std::is_base_of_v<ObjectInterface, T>;

/// Concept for a Java reference. This is a pointer to Java object in C++.
template <class T>
concept JavaReference = std::is_pointer_v<T> && JavaObject<std::remove_pointer_t<T>>;

/// Concept for any type that is compatible with Java objects in their object representation.
/// This should be used in places when doing interop that require the storage/value to be identical to the corresponding
/// Java type.
template <class T>
concept JavaCompatible = std::is_arithmetic_v<T> || std::is_void_v<T> || JavaReference<T>;

// JavaCompatible types convert to themselves.
template <JavaCompatible T>
T javaConvertedType(T);

/// Type alias returning the 'JavaCompatible' type a 'JavaConvertible' type implicitly converts to.
template <class T>
using JavaConvertedType = decltype(javaConvertedType(std::declval<T>()));

/// Concept for any type that is known to implicitly convert to a 'JavaCompatible' type.
template <class T>
concept JavaConvertible = !std::is_void_v<T> && JavaCompatible<JavaConvertedType<T>>;

/// Helper function to call 'fnPtr', which is known to be a Java function, with the given 'args'.
/// Does implicit conversion of 'args' their 'JavaCompatible' type.
template <JavaCompatible Ret, JavaConvertible... Args>
Ret invokeJava(void* fnPtr, Args... args)
{
    return reinterpret_cast<Ret (*)(JavaConvertedType<Args>...)>(fnPtr)(static_cast<JavaConvertedType<Args>>(args)...);
}

} // namespace jllvm
