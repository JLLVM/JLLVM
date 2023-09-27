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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <jllvm/class/Descriptors.hpp>

using namespace jllvm;
using namespace Catch::Matchers;

TEST_CASE("BaseType", "[desc][field]")
{
    auto [enumVal, character, isUnsigned, isInteger] =
        GENERATE(table<BaseType::Values, std::string_view, bool, bool>({{BaseType::Boolean, "Z", true, true},
                                                                        {BaseType::Char, "C", true, true},
                                                                        {BaseType::Float, "F", false, false},
                                                                        {BaseType::Double, "D", false, false},
                                                                        {BaseType::Byte, "B", false, true},
                                                                        {BaseType::Short, "S", false, true},
                                                                        {BaseType::Int, "I", false, true},
                                                                        {BaseType::Long, "J", false, true},
                                                                        {BaseType::Void, "V", false, false}}));

    REQUIRE(FieldType::verify(character));

    // Test with both parsed and assigned from form.
    auto first = FieldType(character);
    auto second = BaseType(enumVal);
    FieldType fieldType = GENERATE_COPY(first, second);

    CHECK(fieldType.textual() == character);
    CHECK(!fieldType.isReference());
    CHECK(fieldType == BaseType(enumVal));

    auto baseType = get_if<BaseType>(&fieldType);
    REQUIRE(baseType);
    CHECK(baseType->getValue() == enumVal);
    CHECK(baseType->isUnsigned() == isUnsigned);
    CHECK(baseType->isIntegerType() == isInteger);
}

TEST_CASE("ObjectType", "[desc][field]")
{
    FieldType fieldType = GENERATE(FieldType("LTest;"), ObjectType("Test"));

    CHECK(fieldType.isReference());
    CHECK(fieldType.textual() == "LTest;");
    CHECK(fieldType == ObjectType("Test"));

    auto objectType = get_if<ObjectType>(&fieldType);
    REQUIRE(objectType);
    CHECK(objectType->getClassName() == "Test");
}

TEST_CASE("ArrayType", "[desc][field]")
{
    FieldType fieldType = GENERATE(FieldType("[I"), ArrayType(BaseType(BaseType::Int)));

    CHECK(fieldType.isReference());
    CHECK(fieldType.textual() == "[I");
    CHECK(fieldType == ArrayType(BaseType(BaseType::Int)));

    auto arrayType = get_if<ArrayType>(&fieldType);
    REQUIRE(arrayType);
    CHECK(arrayType->getComponentType() == BaseType(BaseType::Int));
}

TEST_CASE("FieldType variant methods", "[desc][field]")
{
    auto matchVisitor = [](auto value)
    {
        return [value](auto other)
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::decay_t<decltype(other)>>)
            {
                return value == other;
            }
            return false;
        };
    };

    FieldType fieldType = BaseType(BaseType::Int);
    CHECK(holds_alternative<BaseType>(fieldType));
    CHECK_FALSE(holds_alternative<ObjectType>(fieldType));
    CHECK_FALSE(holds_alternative<ArrayType>(fieldType));
    CHECK(get_if<BaseType>(&fieldType).has_value());
    CHECK(visit(matchVisitor(BaseType(BaseType::Int)), fieldType));

    fieldType = ArrayType(fieldType);
    CHECK_FALSE(holds_alternative<BaseType>(fieldType));
    CHECK_FALSE(holds_alternative<ObjectType>(fieldType));
    CHECK(holds_alternative<ArrayType>(fieldType));
    CHECK(get_if<ArrayType>(&fieldType).has_value());
    CHECK(visit(matchVisitor(ArrayType(BaseType(BaseType::Int))), fieldType));

    fieldType = ObjectType("Bar");
    CHECK_FALSE(holds_alternative<BaseType>(fieldType));
    CHECK(holds_alternative<ObjectType>(fieldType));
    CHECK_FALSE(holds_alternative<ArrayType>(fieldType));
    CHECK(get_if<ObjectType>(&fieldType).has_value());
    CHECK(visit(matchVisitor(ObjectType("Bar")), fieldType));
}

TEST_CASE("FieldType verify", "[desc][field]")
{
    CHECK_FALSE(FieldType::verify(""));
    CHECK_FALSE(FieldType::verify("L"));
    CHECK_FALSE(FieldType::verify("M"));
    CHECK_FALSE(FieldType::verify("["));
    CHECK_FALSE(FieldType::verify("LTest"));
    CHECK_FALSE(FieldType::verify("LTest;A"));
    CHECK_FALSE(FieldType::verify("[;"));
    CHECK_FALSE(FieldType::verify("L;"));
}

TEST_CASE("MethodType parameters and return type", "[desc][method]")
{
    MethodType methodType("()V");
    CHECK_THAT(methodType, SizeIs(0));
    CHECK(methodType.param_begin() == methodType.param_end());
    CHECK(methodType.returnType() == BaseType(BaseType::Void));
    CHECK(methodType.textual() == "()V");

    methodType = MethodType("(IZB)[F");
    CHECK_THAT(methodType, SizeIs(3));
    CHECK(methodType.returnType() == ArrayType(BaseType(BaseType::Float)));
    CHECK(methodType.textual() == "(IZB)[F");
    CHECK_THAT(methodType.parameters(),
               RangeEquals<std::vector<FieldType>>(
                   {BaseType(BaseType::Int), BaseType(BaseType::Boolean), BaseType(BaseType::Byte)}));
}

TEST_CASE("MethodType verify", "[desc][method]")
{
    CHECK_FALSE(MethodType::verify(""));
    CHECK_FALSE(MethodType::verify(")V"));
    CHECK_FALSE(MethodType::verify("("));
    CHECK_FALSE(MethodType::verify("()"));
    CHECK_FALSE(MethodType::verify("(L;)V"));
    CHECK_FALSE(MethodType::verify("(LA)V"));
    CHECK_FALSE(MethodType::verify("([)V"));
    CHECK_FALSE(MethodType::verify("()["));
    CHECK_FALSE(MethodType::verify("()LTest;wdawdwd"));
    CHECK_FALSE(MethodType::verify("(IM)V"));
}
