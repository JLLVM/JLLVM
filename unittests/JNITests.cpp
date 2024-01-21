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

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include <jni.h>

using namespace jllvm;
using namespace Catch::Matchers;

namespace
{

/// Checks that two 'jobject's refer to the same object. JNI defines comparison of two 'jobject's where neither is a
/// nullptr as undefined. 'jniEnv->IsSameObject' must be used instead in these scenarios.
struct IsSameObjectMatcher : Catch::Matchers::MatcherGenericBase
{
    JNIEnv* jniEnv;
    jobject rhs;

    IsSameObjectMatcher(JNIEnv* jniEnv, jobject rhs) : jniEnv(jniEnv), rhs(rhs) {}

    bool match(jobject lhs) const
    {
        return jniEnv->IsSameObject(lhs, rhs);
    }

protected:
    std::string describe() const override
    {
        return "refers to the same object as " + Catch::StringMaker<jobject>::convert(rhs);
    }
};

class VirtualMachineFixture
{
protected:
    VirtualMachine virtualMachine;
    JNIEnv jniEnv;

public:
    VirtualMachineFixture()
        : virtualMachine(VirtualMachine::create(
              []
              {
                  BootOptions bootOptions;
                  bootOptions.classPath = {JAVA_BASE_PATH, INPUTS_BASE_PATH};
                  bootOptions.systemInitialization = false;
                  bootOptions.javaHome = llvm::sys::path::parent_path(llvm::sys::path::parent_path(JAVA_BASE_PATH));
                  return bootOptions;
              }())),
          jniEnv{virtualMachine.getJNINativeInterface()}
    {
    }

    /// Returns an instance of 'IsSameObjectMatcher' comparing a 'jobject' with 'rhs'.
    auto isSameObject(jobject rhs)
    {
        return IsSameObjectMatcher(&jniEnv, rhs);
    }
};
} // namespace

TEST_CASE_METHOD(VirtualMachineFixture, "JNI Get Version", "[JNI]")
{
    CHECK(jniEnv.GetVersion() == JNI_VERSION_10);
}

TEST_CASE_METHOD(VirtualMachineFixture, "JNI FindClass", "[JNI]")
{
    CHECK(jniEnv.FindClass("TestSimpleJNI") != nullptr);
}

TEST_CASE_METHOD(VirtualMachineFixture, "JNI GetStaticFieldID", "[JNI]")
{
    jclass clazz = this->jniEnv.FindClass("TestSimpleJNI");

    jfieldID field = this->jniEnv.GetStaticFieldID(clazz, "instanceI", "I");
    CHECK_FALSE(field);
}

namespace
{
template <char name, auto getter, auto setter>
struct TemplatedFieldVirtualMachineFixture : public VirtualMachineFixture
{
};
} // namespace

/// X-Macro used to conveniently instantiate a test over multiple JNI primitives.
#define NAME_AND_DESCS                                                                                               \
    NAME_DESC(Boolean, 'Z'), NAME_DESC(Byte, 'B'), NAME_DESC(Char, 'C'), NAME_DESC(Short, 'S'), NAME_DESC(Int, 'I'), \
        NAME_DESC(Long, 'J'), NAME_DESC(Float, 'F'), NAME_DESC(Double, 'D')

#define NAME_DESC(name, desc) (desc, &JNIEnv::GetStatic##name##Field, &JNIEnv::SetStatic##name##Field)

TEMPLATE_TEST_CASE_METHOD_SIG(TemplatedFieldVirtualMachineFixture, "JNI Get-Set static", "[JNI]",
                              ((char name, auto getter, auto setter), name, getter, setter), NAME_AND_DESCS,
                              ('O', &JNIEnv::GetStaticObjectField, &JNIEnv::SetStaticObjectField))
{
#undef NAME_DESC

    jclass clazz = this->jniEnv.FindClass("TestSimpleJNI");
    std::string signature = {name};
    char fieldName[2] = {name, 0};
    if (name == 'O')
    {
        signature = "Ljava/lang/String;";
    }

    jfieldID field = this->jniEnv.GetStaticFieldID(clazz, fieldName, signature.c_str());
    REQUIRE(field);

    CHECK((this->jniEnv.*getter)(clazz, field) != 0);

    // NOLINTNEXTLINE(*-use-nullptr): Templated code.
    (this->jniEnv.*setter)(clazz, field, 0);

    CHECK((this->jniEnv.*getter)(clazz, field) == 0);
}

namespace
{
template <auto allocator, auto getter, auto releaser>
struct TemplatedArrayVirtualMachineFixture : public VirtualMachineFixture
{
};
} // namespace

#define NAME_DESC(name, desc) \
    (&JNIEnv::New##name##Array, &JNIEnv::Get##name##ArrayElements, &JNIEnv::Release##name##ArrayElements)

TEMPLATE_TEST_CASE_METHOD_SIG(TemplatedArrayVirtualMachineFixture, "JNI New-Get-Release prim arrays", "[JNI]",
                              ((auto allocator, auto getter, auto releaser), allocator, getter, releaser),
                              NAME_AND_DESCS)
{
#undef NAME_DESC
    constexpr std::size_t length = 5;

    auto array = (this->jniEnv.*allocator)(length);

    CHECK(this->jniEnv.GetArrayLength(array) == length);

    // isCopy parameter should be callable with a nullptr.
    auto* elements = (this->jniEnv.*getter)(array, nullptr);
    // Initially zero init.
    CHECK_THAT(llvm::make_range(elements, elements + length), NoneTrue());

    // Set to all twos.
    std::fill_n(elements, length, 2);

    // Only commit the changes.
    (this->jniEnv.*releaser)(array, elements, JNI_COMMIT);

    // Set to all ones.
    std::fill_n(elements, length, 1);

    // Free and commit the changes.
    (this->jniEnv.*releaser)(array, elements, 0);

    // Get the elements again, check that isCopy can be passed without issues.
    jboolean isCopy;
    elements = (this->jniEnv.*getter)(array, &isCopy);
    CHECK_THAT(llvm::make_range(elements, elements + length), AllTrue());

    // Only dealloc elements.
    (this->jniEnv.*releaser)(array, elements, JNI_ABORT);
}

#define NAME_DESC(name, desc) \
    (&JNIEnv::New##name##Array, &JNIEnv::Get##name##ArrayRegion, &JNIEnv::Set##name##ArrayRegion)

TEMPLATE_TEST_CASE_METHOD_SIG(TemplatedArrayVirtualMachineFixture, "JNI Get-Release-Region prim arrays", "[JNI]",
                              ((auto allocator, auto getter, auto releaser), allocator, getter, releaser),
                              NAME_AND_DESCS)
{
#undef NAME_DESC
    constexpr std::size_t length = 5;
    constexpr std::size_t subsetLength = 3;

    auto array = (this->jniEnv.*allocator)(length);
    // Fetch the element type of the array from the last parameter of the getter which is of type 'T*'.
    using T = std::remove_pointer_t<typename llvm::function_traits<decltype(getter)>::template arg_t<3>>;

    std::vector<T> subset(subsetLength);

    // Only get some elements.
    (this->jniEnv.*getter)(array, 1, subset.size(), subset.data());

    // All zero by default.
    CHECK_THAT(subset, NoneTrue());

    std::fill(subset.begin(), subset.end(), 1);

    // Only fill some elements.
    (this->jniEnv.*releaser)(array, 1, subset.size(), subset.data());

    subset.resize(length);

    // Fetch all elements to check only that only the desired region was affected.
    (this->jniEnv.*getter)(array, 0, subset.size(), subset.data());

    CHECK_THAT(subset, Equals(std::vector<T>{0, 1, 1, 1, 0}));
}

TEST_CASE_METHOD(VirtualMachineFixture, "JNI Object Arrays", "[JNI]")
{
    constexpr std::size_t length = 5;

    jclass classObject = jniEnv.FindClass("java/lang/Class");
    jobjectArray array = jniEnv.NewObjectArray(length, classObject, /*init=*/classObject);
    CHECK(jniEnv.GetArrayLength(array) == length);

    jclass classObjectArray = jniEnv.FindClass("[Ljava/lang/Class;");
    for (std::size_t i : llvm::seq<std::size_t>(0, length))
    {
        // Initially set to classObject.
        CHECK_THAT(jniEnv.GetObjectArrayElement(array, i), isSameObject(classObject));

        jniEnv.SetObjectArrayElement(array, i, classObjectArray);
        CHECK_THAT(jniEnv.GetObjectArrayElement(array, i), isSameObject(classObjectArray));
    }
}
