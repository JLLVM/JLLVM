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
#include <catch2/matchers/catch_matchers_all.hpp>

#include <jllvm/gc/GarbageCollector.hpp>

using namespace jllvm;
using namespace Catch::Matchers;

namespace
{
class GarbageCollectorFixture
{
    llvm::BumpPtrAllocator m_allocator;
    llvm::StringSaver m_stringSaver{m_allocator};

protected:
    GarbageCollector gc{/*heapSize=*/128};
    ClassObject* emptyTestObject;
    ClassObject* arrayOfEmptyTestObject;

public:
    GarbageCollectorFixture()
        : emptyTestObject(ClassObject::create(m_allocator, /*metaClass=*/nullptr, /*vTableSlots=*/0,
                                              /*fieldAreaSize=*/0, /*methods=*/{},
                                              /*fields=*/{}, /*bases=*/{}, "TestObject", false)),
          arrayOfEmptyTestObject(
              ClassObject::createArray(m_allocator, /*metaClass=*/nullptr, emptyTestObject, m_stringSaver))
    {
    }
};
} // namespace

TEST_CASE_METHOD(GarbageCollectorFixture, "Create Object", "[GC]")
{
    Object* objectRaw = gc.allocate(emptyTestObject);
    CHECK(objectRaw->getClass() == emptyTestObject);

    // Rooting the object allows it to persist past a GC.
    GCUniqueRoot object = gc.root(objectRaw);
    CHECK(object->getClass() == emptyTestObject);

    gc.garbageCollect();

    // Memory access remains valid.
    CHECK(object->getClass() == emptyTestObject);
}

TEST_CASE_METHOD(GarbageCollectorFixture, "Create Array", "[GC]")
{
    auto* objectRaw = gc.allocate<Array<Object*>>(arrayOfEmptyTestObject, 4);
    CHECK(objectRaw->getClass() == arrayOfEmptyTestObject);
    CHECK_THAT(*objectRaw, SizeIs(4));

    // Rooting the object allows it to persist past a GC.
    GCUniqueRoot object = gc.root(objectRaw);
    CHECK(object->getClass() == arrayOfEmptyTestObject);
    CHECK_THAT(*object, SizeIs(4));

    for (Object*& element : *object)
    {
        element = gc.allocate(emptyTestObject);
    }

    gc.garbageCollect();

    // Memory access remains valid.
    CHECK(object->getClass() == arrayOfEmptyTestObject);
    CHECK_THAT(*object, SizeIs(4));
    CHECK_THAT(*object, AllMatch(Predicate<Object*>(
                            [&](Object* object) { return object->getClass() == emptyTestObject; }, "can be accessed")));
}

SCENARIO_METHOD(GarbageCollectorFixture, "GCUniqueRoot Behaviour", "[GCUniqueRoot]")
{
    GIVEN("A newly rooted object")
    {
        Object* object = gc.allocate(emptyTestObject);
        GCUniqueRoot root = gc.root(object);

        THEN("the root refers to the object")
        {
            CHECK(root == object);
            CHECK(object == root);
        }

        AND_WHEN("reassigned")
        {
            root = nullptr;
            THEN("refers to the new reference")
            {
                CHECK(root == nullptr);
            }
        }

        THEN("it can be implicitly converted to a GCRootRef")
        {
            STATIC_CHECK(std::is_convertible_v<GCUniqueRoot<Object>, GCRootRef<Object>>);
        }

        THEN("it cannot be assigned a GCRootRef")
        {
            STATIC_CHECK_FALSE(std::is_assignable_v<GCUniqueRoot<Object>, GCRootRef<Object>>);
        }

        AND_WHEN("reset")
        {
            root.reset();
            THEN("it no longer has a root")
            {
                CHECK(root.data() == nullptr);
            }
        }

        AND_WHEN("released")
        {
            GCRootRef ref = root.release();
            THEN("the reference remains")
            {
                gc.garbageCollect();
                CHECK(ref->getClass() == emptyTestObject);
            }
        }
    }
}
