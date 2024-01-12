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

#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include <jni.h>

using namespace jllvm;

namespace
{
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

TEMPLATE_TEST_CASE_METHOD_SIG(TemplatedFieldVirtualMachineFixture, "JNI Get-Set static", "[JNI]",
                              ((char name, auto getter, auto setter), name, getter, setter),
                              ('Z', &JNIEnv::GetStaticBooleanField, &JNIEnv::SetStaticBooleanField),
                              ('O', &JNIEnv::GetStaticObjectField, &JNIEnv::SetStaticObjectField),
                              ('B', &JNIEnv::GetStaticByteField, &JNIEnv::SetStaticByteField),
                              ('C', &JNIEnv::GetStaticCharField, &JNIEnv::SetStaticCharField),
                              ('S', &JNIEnv::GetStaticShortField, &JNIEnv::SetStaticShortField),
                              ('I', &JNIEnv::GetStaticIntField, &JNIEnv::SetStaticIntField),
                              ('J', &JNIEnv::GetStaticLongField, &JNIEnv::SetStaticLongField),
                              ('F', &JNIEnv::GetStaticFloatField, &JNIEnv::SetStaticFloatField),
                              ('D', &JNIEnv::GetStaticDoubleField, &JNIEnv::SetStaticDoubleField))
{
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

TEST_CASE_METHOD(VirtualMachineFixture, "JNI Rooting", "[JNI]")
{
    CHECK(jniEnv.EnsureLocalCapacity(0) == JNI_OK);

    CHECK(jniEnv.PushLocalFrame(16) == JNI_OK);

    jclass clazz = jniEnv.FindClass("TestSimpleJNI");
    REQUIRE(clazz);
    jfieldID field = jniEnv.GetStaticFieldID(clazz, "O", "Ljava/lang/String;");
    REQUIRE(field);
    jobject string = jniEnv.GetStaticObjectField(clazz, field);
    REQUIRE(string);

    // Promote to global ref.
    jobject oldLocalRef = string;
    string = jniEnv.NewGlobalRef(string);
    jniEnv.DeleteLocalRef(oldLocalRef);

    clazz = static_cast<jclass>(jniEnv.PopLocalFrame(clazz));
}
