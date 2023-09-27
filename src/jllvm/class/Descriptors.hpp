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

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/iterator.h>

#include <string_view>

namespace jllvm
{

/// <FieldType> ::= <BaseType> | <ObjectType> | <ArrayType>
///
/// This class acts as a sort 'std::variant' for the three aforementioned alternatives while having a compact inner
/// representation and additional convenience methods.
class FieldType
{
    template <class F>
    friend constexpr decltype(auto) visit(F f, FieldType fieldType);

    friend llvm::hash_code hash_value(FieldType);

    friend struct llvm::DenseMapInfo<FieldType>;

protected:
    // Field sizes specifically chosen given constraints of the JVM spec and to fit in 16 bytes.
    // Array count for 'ArrayTypes'.
    std::uint16_t m_arrayCount{};
    // Integer for 'BaseType'.
    // uint8_t because BaseType::Values is not yet defined.
    std::uint8_t m_baseTypeValue{};
    // Class name for 'ObjectType'.
    // Like string_view members, but smaller.
    std::uint32_t m_size{};
    const char* m_name{};

    // Construct with a BaseType::Values.
    constexpr FieldType(std::uint8_t baseType) : m_baseTypeValue(baseType) {}

    // Construct with an object name.
    constexpr FieldType(std::uint32_t size, const char* name) : m_size(size), m_name(name) {}

public:
    /// Parses a 'FieldType' from a given string.
    constexpr /*implicit*/ FieldType(std::string_view text);

    /// Parses a 'FieldType' from a string literal.
    /// This constructor is 'consteval' and therefore does the parsing (and any errors that may occur!)
    /// at compile time.
    template <std::size_t n>
    consteval /*implicit*/ FieldType(const char (&c)[n]) : FieldType(std::string_view(c, n - 1))
    {
    }

    /// Constructs a 'FieldType' from any of its variants.
    template <class T>
    requires std::is_base_of_v<FieldType, T> constexpr FieldType(T derived)
        : FieldType(static_cast<FieldType&>(derived))
    {
    }

    /// Assignment to a 'FieldType' from any of its variants.
    template <class T>
    requires std::is_base_of_v<FieldType, T> constexpr FieldType& operator=(T derived)
    {
        return *this = static_cast<FieldType&>(derived);
    }

    /// Returns the string representation of the 'FieldType'.
    std::string textual() const;

    constexpr bool operator==(FieldType rhs) const
    {
        return m_arrayCount == rhs.m_arrayCount && m_baseTypeValue == rhs.m_baseTypeValue
               && std::string_view(m_name, m_size) == std::string_view(rhs.m_name, rhs.m_size);
    }

    /// Returns true if this 'FieldType' is a reference type.
    constexpr bool isReference() const;

    /// Returns true if the given string is a valid 'FieldType' descriptor.
    constexpr static bool verify(std::string_view text)
    {
        if (text.empty())
        {
            return false;
        }
        switch (text.front())
        {
            case 'B':
            case 'C':
            case 'D':
            case 'F':
            case 'I':
            case 'J':
            case 'S':
            case 'V':
            case 'Z': return text.size() == 1;
            case '[': return FieldType::verify(text.substr(1));
            case 'L':
            {
                text = text.substr(1);
                auto pos = text.find(';');
                if (pos == std::string_view::npos || pos == 0)
                {
                    return false;
                }
                return pos + 1 == text.size();
            }
            default: return false;
        }
    }
};

/// <BaseType> ::= 'B' | 'C' | 'D' | 'F' | 'I' | 'J' | 'S' | 'Z'
/// Note: We add 'V' for void here as well for convenience.
class BaseType : private FieldType
{
    friend class FieldType;

public:
    enum Values : std::uint8_t
    {
        Boolean = 4, /// 'Z'
        Char = 5,    /// 'C'
        Float = 6,   /// 'F'
        Double = 7,  /// 'D'
        Byte = 8,    /// 'B'
        Short = 9,   /// 'S'
        Int = 10,    /// 'I'
        Long = 11,   /// 'J'
        Void         /// 'V'
    };

    /*implicit*/ constexpr BaseType(Values value) : FieldType(value) {}

    /// Returns the enum value for this base type.
    constexpr Values getValue() const
    {
        return static_cast<Values>(m_baseTypeValue);
    }

    /// Returns true if this base type is an integer type.
    constexpr bool isIntegerType() const
    {
        switch (getValue())
        {
            case Boolean:
            case Byte:
            case Char:
            case Short:
            case Int:
            case Long: return true;
            default: return false;
        }
    }

    /// Returns true if this type is unsigned. All other types are signed.
    constexpr bool isUnsigned() const
    {
        return getValue() == Char || getValue() == Boolean;
    }

    constexpr bool operator==(const BaseType&) const = default;
};

/// <ObjectType> ::= 'L' <ClassName> ';'
class ObjectType : private FieldType
{
    friend class FieldType;

public:
    /// Constructs an 'ObjectType' with the given classname.
    constexpr explicit ObjectType(std::string_view className) : FieldType(className.size(), className.data()) {}

    /// Returns the class name of this 'ObjectType'.
    constexpr std::string_view getClassName() const
    {
        return {m_name, m_size};
    }

    constexpr bool operator==(const ObjectType&) const = default;
};

/// <ArrayType> ::= '[' <FieldType>
class ArrayType : private FieldType
{
    friend class FieldType;

public:
    /// Constructs an 'ArrayType' with the given component type.
    constexpr explicit ArrayType(FieldType componentType) : FieldType(componentType)
    {
        m_arrayCount++;
    }

    /// Returns the component type of this array.
    constexpr FieldType getComponentType() const
    {
        ArrayType copy = *this;
        copy.m_arrayCount--;
        return copy;
    }

    constexpr bool operator==(const ArrayType&) const = default;
};

/// <MethodType> ::= '(' { <FieldType> } ')' <FieldType>
class MethodType
{
    // More compact members to fit into 16 bytes.
    // Since Java descriptors can only be 2^16 bytes large we shouldn't encounter any issues.
    const char* m_text;
    std::uint16_t m_textSize;
    std::uint16_t m_parameterCount{};
    // Offset in 'm_text' where the return type starts.
    std::uint16_t m_retBegin;

    constexpr static std::string_view skipOverField(std::string_view text)
    {
        text = text.substr(text.find_first_not_of('['));
        if (text.front() != 'L')
        {
            return text.substr(1);
        }
        return text.substr(text.find(';') + 1);
    }

public:
    /// Parses a 'MethodType' from a string literal.
    /// This constructor is 'consteval' and therefore does the parsing (and any errors that may occur!)
    /// at compile time.
    template <std::size_t n>
    consteval MethodType(const char (&c)[n]) : MethodType(std::string_view(c, n - 1))
    {
    }

    /// Parses a 'FieldType' from a given string.
    constexpr MethodType(std::string_view text) : m_text(text.data()), m_textSize(text.size())
    {
        // Always check validity eagerly in debug builds or in constant evaluation.
#ifdef NDEBUG
        if (std::is_constant_evaluated())
#endif
        {
            if (!verify(text))
            {
                llvm::report_fatal_error("Invalid method descriptor");
            }
        }

        text = text.substr(1);
        while (text.front() != ')')
        {
            m_parameterCount++;
            text = skipOverField(text);
        }
        text = text.substr(1);
        m_retBegin = text.data() - m_text;
    }

    /// Returns true if the given string is a valid 'MethodType' descriptor.
    constexpr static bool verify(std::string_view text)
    {
        if (text.empty() || text.front() != '(')
        {
            return false;
        }
        text = text.substr(1);
        while (!text.empty() && text.front() != ')')
        {
            auto next = skipOverField(text);
            if (!FieldType::verify(text.substr(0, next.data() - text.data())))
            {
                return false;
            }
            text = next;
        }
        if (text.empty() || text.front() != ')')
        {
            return false;
        }
        text = text.substr(1);

        return FieldType::verify(text);
    }

    /// Iterator over the parameters of a 'MethodType'.
    class param_iterator : public llvm::iterator_facade_base<param_iterator, std::forward_iterator_tag, FieldType,
                                                             std::ptrdiff_t, FieldType*, FieldType>
    {
        std::string_view m_current;

    public:
        param_iterator() = default;

        param_iterator(std::string_view current) : m_current(current) {}

        FieldType operator*() const
        {
            auto temp = skipOverField(m_current);
            return FieldType(m_current.substr(0, temp.data() - m_current.data()));
        }

        param_iterator& operator++()
        {
            m_current = skipOverField(m_current);
            return *this;
        }

        bool operator==(param_iterator rhs) const
        {
            return m_current.data() == rhs.m_current.data();
        }
    };

    /// Returns the parameters of this 'MethodType' in order.
    llvm::iterator_range<param_iterator> parameters() const
    {
        return llvm::make_range(param_begin(), param_end());
    }

    param_iterator param_begin() const
    {
        return param_iterator(std::string_view(m_text + 1, m_textSize - 1));
    }

    param_iterator param_end() const
    {
        return param_iterator(std::string_view(m_text + m_retBegin - 1, m_textSize - m_retBegin + 1));
    }

    /// Returns the number of parameters this 'MethodType' has.
    constexpr std::size_t size() const
    {
        return m_parameterCount;
    }

    /// Returns the return type of this 'MethodType'.
    constexpr FieldType returnType() const
    {
        return FieldType(std::string_view(m_text + m_retBegin, m_textSize - m_retBegin));
    }

    /// Returns the string representation of the 'MethodType'.
    constexpr std::string_view textual() const
    {
        return {m_text, m_textSize};
    }

    constexpr bool operator==(MethodType rhs) const
    {
        return m_retBegin == rhs.m_retBegin && m_parameterCount == rhs.m_parameterCount && textual() == rhs.textual();
    }
};

/// Visitor implementation for 'FieldType' analogous to 'std::visit'.
/// Can be used together with 'match'.
template <class F>
constexpr decltype(auto) visit(F f, FieldType fieldType)
{
    if (fieldType.m_arrayCount > 0)
    {
        // Decrease here since the constructor will increase it again.
        fieldType.m_arrayCount--;
        return f(ArrayType(fieldType));
    }

    if (fieldType.m_name)
    {
        return f(ObjectType({fieldType.m_name, fieldType.m_size}));
    }

    return f(BaseType(static_cast<BaseType::Values>(fieldType.m_baseTypeValue)));
}

/// Returns the instance of the given variant 'T' if active.
/// Undefined behaviour otherwise.
/// Analogous to 'std::get'.
template <class T>
constexpr T get(FieldType fieldType)
{
    return visit(
        []<class U>(U value) -> T
        {
            if constexpr (std::is_same_v<std::decay_t<U>, T>)
            {
                return value;
            }
            llvm_unreachable("FieldType does not contain T");
        },
        fieldType);
}

/// Returns true if 'fieldType' is an instance of 'T'.
/// Analogous to 'std::holds_alternative'.
template <class T>
constexpr bool holds_alternative(FieldType fieldType)
{
    return visit([]<class U>(U) -> bool { return std::is_same_v<std::decay_t<U>, T>; }, fieldType);
}

/// Returns the instance of the given variant 'T' if active or an empty optional otherwise.
/// Analogous to 'std::get_if'.
template <class T>
constexpr std::optional<T> get_if(const FieldType* fieldType)
{
    if (!fieldType)
    {
        return std::nullopt;
    }
    return holds_alternative<T>(*fieldType) ? std::optional{get<T>(*fieldType)} : std::nullopt;
}

inline llvm::hash_code hash_value(FieldType fieldType)
{
    return llvm::hash_combine(fieldType.m_arrayCount, fieldType.m_baseTypeValue,
                              llvm::StringRef{fieldType.m_name, fieldType.m_size});
}

inline llvm::hash_code hash_value(MethodType methodType)
{
    return llvm::hash_value(methodType.textual());
}

constexpr FieldType::FieldType(std::string_view text) : m_arrayCount(text.find_first_not_of('['))
{
    // Always check validity eagerly in debug builds or in constant evaluation.
#ifdef NDEBUG
    if (std::is_constant_evaluated())
#endif
    {
        if (!verify(text))
        {
            llvm::report_fatal_error("Invalid field descriptor");
        }
    }

    text = text.substr(m_arrayCount);
    switch (text.front())
    {
        case 'B': m_baseTypeValue = BaseType::Byte; break;
        case 'C': m_baseTypeValue = BaseType::Char; break;
        case 'D': m_baseTypeValue = BaseType::Double; break;
        case 'F': m_baseTypeValue = BaseType::Float; break;
        case 'I': m_baseTypeValue = BaseType::Int; break;
        case 'J': m_baseTypeValue = BaseType::Long; break;
        case 'S': m_baseTypeValue = BaseType::Short; break;
        case 'V': m_baseTypeValue = BaseType::Void; break;
        case 'Z': m_baseTypeValue = BaseType::Boolean; break;
        case 'L':
        {
            text = text.substr(1);
            m_size = text.find(';');
            m_name = text.data();
            break;
        }
    }
}

constexpr bool FieldType::isReference() const
{
    return holds_alternative<ObjectType>(*this) || holds_alternative<ArrayType>(*this);
}

} // namespace jllvm

namespace llvm
{
template <>
struct DenseMapInfo<jllvm::FieldType>
{
    static jllvm::FieldType getEmptyKey()
    {
        return jllvm::FieldType(-1);
    }

    static jllvm::FieldType getTombstoneKey()
    {
        return jllvm::FieldType(-2);
    }

    static unsigned getHashValue(jllvm::FieldType fieldType)
    {
        return hash_value(fieldType);
    }

    static bool isEqual(jllvm::FieldType lhs, jllvm::FieldType rhs)
    {
        return lhs == rhs;
    }
};
} // namespace llvm
