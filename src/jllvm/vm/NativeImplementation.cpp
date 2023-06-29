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

#include "NativeImplementation.hpp"

#include <jllvm/vm/native/IO.hpp>
#include <jllvm/vm/native/JDK.hpp>
#include <jllvm/vm/native/Lang.hpp>
#include <jllvm/vm/native/Security.hpp>

void jllvm::registerJavaClasses(VirtualMachine& virtualMachine)
{
    using namespace lang;
    using namespace jdk;
    using namespace security;
    using namespace io;

    addModels<ObjectModel, ClassModel, ThrowableModel, FloatModel, DoubleModel, SystemModel, ReflectionModel, CDSModel,
              UnsafeModel, VMModel, ReferenceModel, SystemPropsRawModel, RuntimeModel, FileDescriptorModel,
              ScopedMemoryAccessModel, SignalModel, ThreadModel, AccessControllerModel, FileOutputStreamModel>(virtualMachine);
}
