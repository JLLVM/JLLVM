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
auto getStaticFieldFunction()
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
auto setStaticFieldFunction()
{
    return translateJNIInterface(
        [](VirtualMachine&, GCRootRef<ClassObject>, Field* field, T value)
        {
            using type = std::conditional_t<std::is_same_v<T, GCRootRef<ObjectInterface>>, ObjectInterface*, T>;
            auto staticField = StaticFieldRef<type>(field);
            staticField() = value;
        });
}

/// Implementation of New*Array for type 'T' corresponding with the given descriptor.
template <class T, const BaseType& descriptor>
auto newPrimitiveArrayFunction()
{
    return translateJNIInterface(
        [](VirtualMachine& virtualMachine, std::int32_t length)
        {
            ClassObject& classObject = virtualMachine.getClassLoader().forName(ArrayType(descriptor));
            return virtualMachine.getGC().allocate<Array<T>>(&classObject, length);
        });
}

template <class T>
constexpr auto getPrimitiveArrayElementsLambda = [](VirtualMachine&, GCRootRef<Array<T>> array, jboolean* isCopy)
{
    // The GC does not yet have support for object pinning.
    // Always create a copy to deal with relocations.
    if (isCopy)
    {
        *isCopy = true;
    }
    auto* storage = new T[array->size()];
    llvm::copy(*array, storage);
    return storage;
};

/// Implementation of Get*ArrayElements.
template <class T>
auto getPrimitiveArrayElementsFunction()
{
    return translateJNIInterface(getPrimitiveArrayElementsLambda<T>);
}

template <class T>
constexpr auto releasePrimitiveArrayElementsLambda =
    [](VirtualMachine&, GCRootRef<Array<T>> array, T* elements, jint mode)
{
    if (mode != JNI_ABORT)
    {
        llvm::copy(llvm::make_range(elements, elements + array->size()), array->begin());
    }
    if (mode != JNI_COMMIT)
    {
        delete[] elements;
    }
};

/// Implementation of Release*ArrayElements.
template <class T>
auto releasePrimitiveArrayElementsFunction()
{
    return translateJNIInterface(releasePrimitiveArrayElementsLambda<T>);
}

/// Implementation of Get*ArrayRegion.
template <class T>
auto getPrimitiveArrayRegionFunction()
{
    return translateJNIInterface([](VirtualMachine&, GCRootRef<Array<T>> array, jsize start, jsize len, T* elements)
                                 { std::copy_n(array->begin() + start, len, elements); });
}

/// Implementation of Set*ArrayRegion.
template <class T>
auto setPrimitiveArrayRegionFunction()
{
    return translateJNIInterface([](VirtualMachine&, GCRootRef<Array<T>> array, jsize start, jsize len,
                                    const T* elements) { std::copy_n(elements, len, array->begin() + start); });
}

} // namespace

/// X-Macro used to conveniently implement JNI methods for primitives that only differ in part of the name and type.
#define NAME_AND_TYPES_PRIMS     \
    NAME_TYPE(Boolean, jboolean) \
    NAME_TYPE(Byte, jbyte)       \
    NAME_TYPE(Char, jchar)       \
    NAME_TYPE(Short, jshort)     \
    NAME_TYPE(Int, jint)         \
    NAME_TYPE(Long, jlong)       \
    NAME_TYPE(Float, jfloat)     \
    NAME_TYPE(Double, jdouble)

/// X-Macro used to conveniently implement JNI methods that only differs in part of the name and type.
#define NAME_AND_TYPES   \
    NAME_AND_TYPES_PRIMS \
    NAME_TYPE(Object, GCRootRef<ObjectInterface>)

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
    result->IsSameObject = translateJNIInterface([](VirtualMachine&, GCRootRef<ObjectInterface> lhs,
                                                    GCRootRef<ObjectInterface> rhs) -> jboolean { return lhs == rhs; });

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

    result->GetArrayLength =
        translateJNIInterface([](VirtualMachine&, GCRootRef<AbstractArray> array) -> jsize { return array->size(); });
    result->NewObjectArray = translateJNIInterface(
        [](VirtualMachine& virtualMachine, std::int32_t length, GCRootRef<ClassObject> elementClass,
           GCRootRef<ObjectInterface> element)
        {
            ClassObject& classObject =
                virtualMachine.getClassLoader().forName(ArrayType(elementClass->getDescriptor()));
            auto* array = virtualMachine.getGC().allocate<Array<ObjectInterface*>>(&classObject, length);
            std::fill(array->begin(), array->end(), element);
            return array;
        });
    result->GetObjectArrayElement = translateJNIInterface(
        [](VirtualMachine&, GCRootRef<Array<ObjectInterface*>> array, std::int32_t index) { return (*array)[index]; });
    result->SetObjectArrayElement =
        translateJNIInterface([](VirtualMachine&, GCRootRef<Array<ObjectInterface*>> array, std::int32_t index,
                                 GCRootRef<ObjectInterface> value) { (*array)[index] = value; });

#define NAME_TYPE(name, type)                                                     \
    {                                                                             \
        constexpr static BaseType descriptor = BaseType::name;                    \
        result->New##name##Array = newPrimitiveArrayFunction<type, descriptor>(); \
    }
    NAME_AND_TYPES_PRIMS
#undef NAME_TYPE

#define NAME_TYPE(name, type) result->Get##name##ArrayElements = getPrimitiveArrayElementsFunction<type>();
    NAME_AND_TYPES_PRIMS
#undef NAME_TYPE

#define NAME_TYPE(name, type) result->Release##name##ArrayElements = releasePrimitiveArrayElementsFunction<type>();
    NAME_AND_TYPES_PRIMS
#undef NAME_TYPE

#define NAME_TYPE(name, type) result->Get##name##ArrayRegion = getPrimitiveArrayRegionFunction<type>();
    NAME_AND_TYPES_PRIMS
#undef NAME_TYPE

#define NAME_TYPE(name, type) result->Set##name##ArrayRegion = setPrimitiveArrayRegionFunction<type>();
    NAME_AND_TYPES_PRIMS
#undef NAME_TYPE

    // These are more constrained versions of the '(Get|Release)*ArrayElements' making it more likely for the VM to
    // return a pointer to the array elements. Performing a copy here by falling back to the normal version is a valid
    // implementation.
    result->GetPrimitiveArrayCritical = translateJNIInterface(
        [](VirtualMachine& virtualMachine, GCRootRef<AbstractArray> array, jboolean* isCopy)
        {
            return selectForJVMType(array->getClass()->getComponentType()->getDescriptor(),
                                    [&](auto primitive) -> void*
                                    {
                                        using T = std::remove_cvref_t<decltype(primitive)>;
                                        if constexpr (std::is_same_v<T, ObjectInterface*>)
                                        {
                                            llvm_unreachable("not possible");
                                        }
                                        else
                                        {
                                            return getPrimitiveArrayElementsLambda<T>(
                                                virtualMachine, static_cast<GCRootRef<Array<T>>>(array), isCopy);
                                        }
                                    });
        });
    result->ReleasePrimitiveArrayCritical = translateJNIInterface(
        [](VirtualMachine& virtualMachine, GCRootRef<AbstractArray> array, void* carray, jint mode)
        {
            return selectForJVMType(array->getClass()->getComponentType()->getDescriptor(),
                                    [&](auto primitive)
                                    {
                                        using T = std::remove_cvref_t<decltype(primitive)>;
                                        if constexpr (std::is_same_v<T, ObjectInterface*>)
                                        {
                                            llvm_unreachable("not possible");
                                        }
                                        else
                                        {
                                            releasePrimitiveArrayElementsLambda<T>(
                                                virtualMachine, static_cast<GCRootRef<Array<T>>>(array),
                                                reinterpret_cast<T*>(carray), mode);
                                        }
                                    });
        });

    return JNINativeInterfaceUPtr(result, +[](JNINativeInterface_* ptr) { delete ptr; });
}
