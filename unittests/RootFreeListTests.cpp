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

using namespace jllvm;

TEST_CASE("Iterators", "[RootFreeList]")
{
    RootFreeList list(/*slabSize=*/2);
    CHECK(list.begin() == list.end());

    // Allocate some more roots to require more than one slab.
    list.allocate();
    list.allocate();
    // Exactly 4 elements, so that both slabs are full too.
    list.allocate();
    list.allocate();

    CHECK(std::distance(list.begin(), list.end()) == 4);
}

TEST_CASE("Free, optimised pattern", "[RootFreeList]")
{
    RootFreeList list(/*slabSize=*/2);

    // Allocate some more roots to require more than one slab.
    GCRootRef first = list.allocate();
    first = reinterpret_cast<ObjectInterface*>(1);
    GCRootRef second = list.allocate();
    second = reinterpret_cast<ObjectInterface*>(2);
    GCRootRef third = list.allocate();
    third = reinterpret_cast<ObjectInterface*>(3);
    GCRootRef fourth = list.allocate();
    fourth = reinterpret_cast<ObjectInterface*>(4);

    list.free(fourth);
    list.free(third);
    CHECK(second == reinterpret_cast<ObjectInterface*>(2));
    CHECK(first == reinterpret_cast<ObjectInterface*>(1));

    list.free(list.allocate());
    list.free(second);
    list.free(first);

    CHECK(list.begin() == list.end());
}

TEST_CASE("Free, not optimal pattern", "[RootFreeList]")
{
    RootFreeList list(/*slabSize=*/2);

    // Allocate some more roots to require more than one slab.
    GCRootRef first = list.allocate();
    first = reinterpret_cast<ObjectInterface*>(1);
    GCRootRef second = list.allocate();
    second = reinterpret_cast<ObjectInterface*>(2);
    GCRootRef third = list.allocate();
    third = reinterpret_cast<ObjectInterface*>(3);
    GCRootRef fourth = list.allocate();
    fourth = reinterpret_cast<ObjectInterface*>(4);

    list.free(third);
    list.free(second);
    CHECK(fourth == reinterpret_cast<ObjectInterface*>(4));
    CHECK(first == reinterpret_cast<ObjectInterface*>(1));

    list.free(list.allocate());
    list.free(first);
    list.free(fourth);

    CHECK(list.begin() == list.end());

    // Make sure it syncs up with the end of the free list again and can allocate new slabs.
    list.allocate();
    list.allocate();
    list.allocate();
    list.allocate();
    list.allocate();
    list.allocate();
    GCRootRef last = list.allocate();
    last = reinterpret_cast<ObjectInterface*>(8);

    CHECK(std::distance(list.begin(), list.end()) == 7);
}
