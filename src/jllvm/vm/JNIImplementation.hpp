
#pragma once

#include <jni.h>

#include "VirtualMachine.hpp"

namespace jllvm
{
/// Returns the 'VirtualMachine' instance associated with the 'JNIEnv'.
VirtualMachine& virtualMachineFromJNIEnv(JNIEnv* env);
} // namespace jllvm
