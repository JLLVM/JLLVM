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

#include <jllvm/gc/RootFreeList.hpp>

#include "jllvm/object/Object.hpp"

using namespace jllvm;

SCENARIO("GCRootRef Behaviour", "[GCRootRef]")
{
    GIVEN("A newly created GCRootRef")
    {
        RootFreeList list(/*slabSize=*/2);
        GCRootRef ref = list.allocate();

        THEN("It refers to no object")
        {
            CHECK(ref == nullptr);
            CHECK(nullptr == ref);
            CHECK(!ref);
        }

        AND_WHEN("assigned an object")
        {
            Object object(nullptr);
            ref = &object;

            THEN("it can be retrieved")
            {
                CHECK(ref->getClass() == object.getClass());
                CHECK((*ref).getClass() == object.getClass());
            }

            THEN("it evaluates to true")
            {
                CHECK(ref);
            }

            THEN("it compares equal to the object")
            {
                CHECK(ref == &object);
                CHECK(&object == ref);
            }

            THEN("it does not equal null")
            {
                CHECK(ref != nullptr);
                CHECK(nullptr != ref);
            }
        }
    }

    GIVEN("Two roots to the same object")
    {
        RootFreeList list(/*slabSize=*/2);
        GCRootRef first = list.allocate();
        GCRootRef second = list.allocate();
        Object object(nullptr);
        first = &object;
        second = &object;

        THEN("They compare equal")
        {
            CHECK(first == second);
        }
    }

    GIVEN("A root to a base class")
    {
        RootFreeList list(/*slabSize=*/2);
        GCRootRef<ObjectInterface> root = list.allocate();

        THEN("It can be explicitly downcast")
        {
            auto array = static_cast<GCRootRef<Array<int>>>(root);
            CHECK(array == root);
        }

        THEN("It cannot be implicitly downcast")
        {
            STATIC_CHECK_FALSE(std::is_convertible_v<GCRootRef<ObjectInterface>, GCRootRef<Array<int>>>);
        }
    }

    GIVEN("A root a derived class")
    {
        RootFreeList list(/*slabSize=*/2);
        GCRootRef<ObjectInterface> root = list.allocate();
        auto array = static_cast<GCRootRef<Array<int>>>(root);

        THEN("It can be implicitly upcast")
        {
            root = array;
            CHECK(array == root);
        }
    }
}
