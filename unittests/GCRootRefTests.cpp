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
    STATIC_CHECK(std::is_trivially_copyable_v<GCRootRef<ObjectInterface>>);

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
            ref.assign(&object);

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

        AND_WHEN("assigned null")
        {
            GCRootRef copy = ref;
            Object object(nullptr);
            copy.assign(&object);
            ref = nullptr;
            THEN("it does not write to the root")
            {
                CHECK(copy == &object);
                CHECK(ref == nullptr);
            }
            THEN("it no longer refers to the root")
            {
                CHECK(ref != copy);
                CHECK(ref.data() != copy.data());
                CHECK(ref.data() == nullptr);
            }
        }
    }

    GIVEN("Two roots to the same object")
    {
        RootFreeList list(/*slabSize=*/2);
        GCRootRef first = list.allocate();
        GCRootRef second = list.allocate();
        Object object(nullptr);
        first.assign(&object);
        second.assign(&object);

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

    GIVEN("An empty GCRootRef")
    {
        RootFreeList list(/*slabSize=*/2);

        GCRootRef<ObjectInterface> root;
        THEN("It implicitly converts to null")
        {
            CHECK(root == nullptr);
        }
        THEN("It compares equal with other null roots")
        {
            CHECK(root == root);
            CHECK(root == GCRootRef<ObjectInterface>());
        }
        THEN("It compares equal with roots to null")
        {
            GCRootRef<ObjectInterface> other = list.allocate();
            CHECK(other == root);
        }
        THEN("It evaluates to false")
        {
            CHECK_FALSE(root);
            CHECK(!root);
        }
        THEN("Its address is null")
        {
            CHECK(root.address() == nullptr);
        }
        THEN("Its data is null")
        {
            CHECK(root.data() == nullptr);
        }
    }
}
