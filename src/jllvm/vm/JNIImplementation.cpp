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

#include "JNIImplementation.hpp"

#include "VirtualMachine.hpp"

jllvm::VirtualMachine& jllvm::virtualMachineFromJNIEnv(JNIEnv* env)
{
    return *reinterpret_cast<jllvm::VirtualMachine*>(env->functions->reserved0);
}

namespace
{

using namespace jllvm;

/// Implementation of 'GetStatic*Field' that should return type 'T'.
template <class T>
consteval auto getStaticFieldFunction()
{
    return translateJNIInterface(
        [](VirtualMachine&, GCRootRef<ClassObject>, Field* field)
        {
            using type = std::conditional_t<std::is_same_v<T, GCRootRef<ObjectInterface>>, ObjectInterface*, T>;
            return static_cast<type>(StaticFieldRef<type>(field)());
        });
}

/// Implementation of 'SetStatic*Field' that set a value of type 'T'.
template <class T>
consteval auto setStaticFieldFunction()
{
    return translateJNIInterface(
        [](VirtualMachine&, GCRootRef<ClassObject>, Field* field, T value)
        {
            using type = std::conditional_t<std::is_same_v<T, GCRootRef<ObjectInterface>>, ObjectInterface*, T>;
            auto staticField = StaticFieldRef<type>(field);
            staticField() = value;
        });
}

} // namespace

/// X-Macro used to conveniently implement JNI methods that only differs in part of the name and type.
#define NAME_AND_TYPES                            \
    NAME_TYPE(Boolean, jboolean)                  \
    NAME_TYPE(Object, GCRootRef<ObjectInterface>) \
    NAME_TYPE(Byte, jbyte)                        \
    NAME_TYPE(Char, jchar)                        \
    NAME_TYPE(Short, jshort)                      \
    NAME_TYPE(Int, jint)                          \
    NAME_TYPE(Long, jlong)                        \
    NAME_TYPE(Float, jfloat)                      \
    NAME_TYPE(Double, jdouble)

jllvm::VirtualMachine::JNINativeInterfaceUPtr jllvm::VirtualMachine::createJNIEnvironment()
{
    auto* result = new JNINativeInterface_{};
    result->reserved0 = reinterpret_cast<void*>(this);

    result->GetVersion = +[](JNIEnv*) -> jint { return JNI_VERSION_10; };
    result->FindClass = +[](JNIEnv* env, const char* name) -> jclass
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        ClassObject& classObject = virtualMachine.getClassLoader().forName(FieldType::fromMangled(name));
        return llvm::bit_cast<jclass>(virtualMachine.getGC().root(&classObject).release());
    };

    result->GetStaticFieldID = translateJNIInterface(
        [](VirtualMachine& virtualMachine, GCRootRef<ClassObject> classObject, const char* name,
           const char* signature) -> const Field*
        {
            // Static field access always initializes the class object.
            virtualMachine.initialize(*classObject);
            return classObject->getStaticField(name, FieldType(signature));
        });

#define NAME_TYPE(name, type) result->GetStatic##name##Field = getStaticFieldFunction<type>();
    NAME_AND_TYPES
#undef NAME_TYPE

#define NAME_TYPE(name, type) result->SetStatic##name##Field = setStaticFieldFunction<type>();
    NAME_AND_TYPES
#undef NAME_TYPE

    return JNINativeInterfaceUPtr(result, +[](JNINativeInterface_* ptr) { delete ptr; });
}
