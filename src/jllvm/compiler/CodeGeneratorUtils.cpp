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

#include "CodeGeneratorUtils.hpp"

using namespace jllvm;

namespace
{

bool isCategoryTwo(ByteCodeTypeChecker::JVMType type)
{
    return type.is<llvm::Type*>()
           && (type.get<llvm::Type*>()->isIntegerTy(64) || type.get<llvm::Type*>()->isDoubleTy());
}

template <class... Args>
struct OneOfBase : ByteCodeBase
{
    template <class T, class = std::enable_if_t<(std::is_same_v<std::decay_t<T>, Args> || ...)>>
    OneOfBase(T&& value) : ByteCodeBase(std::forward<T>(value))
    {
    }
};
} // namespace

void ByteCodeTypeChecker::checkBasicBlock(llvm::ArrayRef<char> block, std::uint16_t offset, TypeStack typeStack)
{
    bool done = false;

    auto pushNext = [&](std::uint16_t next)
    {
        if (m_basicBlocks.insert({next, typeStack}).second)
        {
            m_offsetStack.push_back(next);
        }
    };
    auto checkRet = [&](auto& ret)
    {
        std::uint16_t retAddress = m_locals[ret.index].template get<RetAddrType>();
        m_subroutineToReturnInfoMap.insert(
            {m_returnAddressToSubroutineMap[retAddress], {static_cast<std::uint16_t>(ret.offset), retAddress}});

        pushNext(retAddress);
        done = true;
    };
    auto checkStore = [&](auto& store)
    {
        JVMType type = typeStack.back();
        typeStack.pop_back();
        m_locals[store.index] = type;
    };

    for (ByteCodeOp operation : byteCodeRange(block, offset))
    {
        if (done)
        {
            return;
        }

        if (getOffset(operation) == m_byteCodeTypeInfo.offset)
        {
            m_byteCodeTypeInfo.operandStack = typeStack;
            m_byteCodeTypeInfo.locals = m_locals;
        }

        match(
            operation, [](...) { llvm_unreachable("NOT YET IMPLEMENTED"); },
            [&](OneOfBase<AALoad, ANewArray, NewArray>)
            {
                if (holds_alternative<AALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_addressType;
            },
            [&](OneOfBase<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
            { typeStack.erase(typeStack.end() - 3, typeStack.end()); },
            [&](OneOfBase<AConstNull, ALoad, ALoad0, ALoad1, ALoad2, ALoad3, New>)
            { typeStack.emplace_back(m_addressType); },
            [&](OneOfBase<AReturn, AThrow, DReturn, FReturn, IReturn, LReturn, Return>) { done = true; },
            [&](OneOf<AStore, IStore, FStore, DStore, LStore> store) { checkStore(store); },
            [&](OneOf<AStore0, AStore1, AStore2, AStore3, IStore0, IStore1, IStore2, IStore3, FStore0, FStore1, FStore2,
                      FStore3, DStore0, DStore1, DStore2, DStore3, LStore0, LStore1, LStore2, LStore3>)
            {
                JVMType type = typeStack.back();
                typeStack.pop_back();

                auto index = match(
                    operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                    [&](OneOf<AStore0, IStore0, FStore0, DStore0, LStore0>) { return 0; },
                    [&](OneOf<AStore1, IStore1, FStore1, DStore1, LStore1>) { return 1; },
                    [&](OneOf<AStore2, IStore2, FStore2, DStore2, LStore2>) { return 2; },
                    [&](OneOf<AStore3, IStore3, FStore3, DStore3, LStore3>) { return 3; });

                m_locals[index] = type;
            },
            [&](OneOfBase<ArrayLength, D2I, F2I, InstanceOf, L2I>) { typeStack.back() = m_intType; },
            [&](OneOfBase<CheckCast, DNeg, FNeg, I2B, I2C, I2S, IInc, INeg, LNeg, Nop>) { /* Types do not change */ },
            [&](OneOfBase<BALoad, CALoad, DCmpG, DCmpL, FCmpG, FCmpL, IALoad, LCmp, SALoad>)
            {
                typeStack.pop_back();
                typeStack.back() = m_intType;
            },
            [&](OneOfBase<BIPush, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4, IConst5, ILoad, ILoad0, ILoad1,
                          ILoad2, ILoad3, SIPush>) { typeStack.emplace_back(m_intType); },
            [&](OneOfBase<D2F, I2F, L2F, FALoad>)
            {
                if (holds_alternative<FALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_floatType;
            },
            [&](OneOfBase<D2L, F2L, I2L, LALoad>)
            {
                if (holds_alternative<LALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_longType;
            },
            [&](OneOfBase<DAdd, DDiv, DMul, DRem, DSub, FAdd, FDiv, FMul, FRem, FSub, IAdd, IAnd, IDiv, IMul, IOr, IRem,
                          IShl, IShr, ISub, IUShr, IXor, LAdd, LAnd, LDiv, LMul, LOr, LRem, LShl, LShr, LSub, LUShr,
                          LXor, MonitorEnter, MonitorExit, Pop, PutStatic>) { typeStack.pop_back(); },
            [&](OneOfBase<DALoad, F2D, I2D, L2D>)
            {
                if (holds_alternative<DALoad>(operation))
                {
                    typeStack.pop_back();
                }
                typeStack.back() = m_doubleType;
            },
            [&](OneOfBase<DConst0, DConst1, DLoad, DLoad0, DLoad1, DLoad2, DLoad3>)
            { typeStack.emplace_back(m_doubleType); },
            [&](Dup) { typeStack.push_back(typeStack.back()); },
            [&](DupX1)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                assert(!isCategoryTwo(type1) && !isCategoryTwo(type2));

                typeStack.insert(iter.base(), type1);
            },
            [&](DupX2)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type2))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                    ++iter;
                }

                typeStack.insert(iter.base(), type1);
            },
            [&](Dup2)
            {
                auto iter = typeStack.rbegin();
                JVMType type = *iter++;

                if (!isCategoryTwo(type))
                {
                    // Form 1: where both value1 and value2 are values of a category 1 computational type
                    JVMType type2 = *iter++;

                    typeStack.push_back(type2);
                }

                typeStack.push_back(type);
            },
            [&](Dup2X1)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type

                    typeStack.insert((++iter).base(), {type2, type1});
                }
                else
                {
                    // Form 2: where value1 is a value of a category 2 computational type and value2 is a value of a
                    // category 1 computational type
                    typeStack.insert(iter.base(), type1);
                }
            },
            [&](Dup2X2)
            {
                auto iter = typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    JVMType type3 = *iter++;

                    if (!isCategoryTwo(type3))
                    {
                        // Form 1: where value1, value2, value3, and value4 are all values of a category 1 computational
                        // type
                        ++iter;
                    }

                    typeStack.insert(iter.base(), {type2, type1});
                }
                else
                {
                    if (!isCategoryTwo(type2))
                    {
                        // Form 2: where value1 is a value of a category 2 computational type and value2 and value3 are
                        // both values of a category 1 computational type
                        ++iter;
                    }

                    typeStack.insert(iter.base(), type1);
                }
            },
            [&](OneOfBase<FConst0, FConst1, FConst2, FLoad, FLoad0, FLoad1, FLoad2, FLoad3>)
            { typeStack.emplace_back(m_floatType); },
            [&](OneOf<GetField, GetStatic> get)
            {
                if (holds_alternative<GetField>(operation))
                {
                    typeStack.pop_back();
                }

                auto descriptor = FieldType(PoolIndex<FieldRefInfo>{get.index}
                                                .resolve(m_classFile)
                                                ->nameAndTypeIndex.resolve(m_classFile)
                                                ->descriptorIndex.resolve(m_classFile)
                                                ->text);

                llvm::Type* type = descriptorToType(descriptor, m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                typeStack.emplace_back(type);
            },
            [&](OneOf<Goto, GotoW> gotoOp)
            {
                pushNext(gotoOp.offset + gotoOp.target);
                done = true;
            },
            [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                      IfGe, IfGt, IfLe, IfNonNull, IfNull>
                    cmpOp)
            {
                typeStack.pop_back();

                match(
                    operation,
                    [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                    { typeStack.pop_back(); },
                    [](...) {});

                pushNext(cmpOp.offset + cmpOp.target);
                pushNext(cmpOp.offset + sizeof(OpCodes) + sizeof(std::int16_t));
                done = true;
            },
            // TODO InvokeDynamic
            [&](OneOf<InvokeInterface, InvokeSpecial, InvokeStatic, InvokeVirtual> invoke)
            {
                auto descriptor = MethodType(PoolIndex<RefInfo>{invoke.index}
                                                 .resolve(m_classFile)
                                                 ->nameAndTypeIndex.resolve(m_classFile)
                                                 ->descriptorIndex.resolve(m_classFile)
                                                 ->text);

                for (std::size_t i = 0; i < descriptor.size(); i++)
                {
                    typeStack.pop_back();
                }

                // static does not pop this
                if (!holds_alternative<InvokeStatic>(operation))
                {
                    typeStack.pop_back();
                }

                llvm::Type* type = descriptorToType(descriptor.returnType(), m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                if (!type->isVoidTy())
                {
                    typeStack.emplace_back(type);
                }
            },
            [&](OneOf<JSR, JSRw> jsr)
            {
                std::uint16_t retAddress =
                    jsr.offset + sizeof(OpCodes)
                    + (holds_alternative<JSRw>(operation) ? sizeof(std::int32_t) : sizeof(std::int16_t));
                std::uint16_t target = jsr.offset + jsr.target;

                m_returnAddressToSubroutineMap.insert({retAddress, target});

                // check if the subroutine has already been type-checked. If so use the previously calculated typeStack
                if (auto iter = m_subroutineToReturnInfoMap.find(target); iter != m_subroutineToReturnInfoMap.end())
                {
                    typeStack = m_basicBlocks[iter->second.returnAddress];
                    pushNext(retAddress);
                }
                else
                {
                    typeStack.emplace_back(retAddress);
                    pushNext(target);
                }

                done = true;
            },
            [&](OneOfBase<LConst0, LConst1, LLoad, LLoad0, LLoad1, LLoad2, LLoad3>)
            { typeStack.emplace_back(m_longType); },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(m_classFile), [&](const ClassInfo*) { typeStack.emplace_back(m_addressType); },
                    [&](const DoubleInfo*) { typeStack.emplace_back(m_doubleType); },
                    [&](const FloatInfo*) { typeStack.emplace_back(m_floatType); },
                    [&](const IntegerInfo*) { typeStack.emplace_back(m_intType); },
                    [&](const LongInfo*) { typeStack.emplace_back(m_longType); },
                    [&](const StringInfo*) { typeStack.emplace_back(m_addressType); },
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
            },
            [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
            {
                typeStack.pop_back();

                pushNext(switchOp.offset + switchOp.defaultOffset);

                for (std::int32_t target : llvm::make_second_range(switchOp.matchOffsetsPairs))
                {
                    pushNext(switchOp.offset + target);
                }
                done = true;
            },
            [&](MultiANewArray multiANewArray)
            {
                for (int i = 0; i < multiANewArray.dimensions; ++i)
                {
                    typeStack.pop_back();
                }

                typeStack.emplace_back(m_addressType);
            },
            [&](Pop2)
            {
                JVMType type = typeStack.back();
                typeStack.pop_back();
                if (!isCategoryTwo(type))
                {
                    typeStack.pop_back();
                }
            },
            [&](PutField)
            {
                typeStack.pop_back();
                typeStack.pop_back();
            },
            [&](Ret ret) { checkRet(ret); }, [&](Swap) { std::swap(typeStack.back(), *std::next(typeStack.rbegin())); },
            [&](Wide wide)
            {
                llvm::Type* type;
                switch (wide.opCode)
                {
                    default: llvm_unreachable("Invalid wide operation");
                    case OpCodes::AStore:
                    case OpCodes::DStore:
                    case OpCodes::FStore:
                    case OpCodes::IStore:
                    case OpCodes::LStore:
                    {
                        checkStore(wide);
                        return;
                    }
                    case OpCodes::Ret:
                    {
                        checkRet(wide);
                        return;
                    }
                    case OpCodes::IInc:
                    {
                        return;
                    }
                    case OpCodes::ALoad:
                    {
                        type = m_addressType;
                        break;
                    }
                    case OpCodes::DLoad:
                    {
                        type = m_doubleType;
                        break;
                    }
                    case OpCodes::FLoad:
                    {
                        type = m_floatType;
                        break;
                    }
                    case OpCodes::ILoad:
                    {
                        type = m_intType;
                        break;
                    }
                    case OpCodes::LLoad:
                    {
                        type = m_longType;
                        break;
                    }
                }

                typeStack.emplace_back(type);
            });
    }
}

void ByteCodeTypeChecker::check()
{
    for (const auto& exception : m_code.getExceptionTable())
    {
        if (m_basicBlocks.insert({exception.handlerPc, {m_addressType}}).second)
        {
            m_offsetStack.push_back(exception.handlerPc);
        }
    }

    m_basicBlocks.insert({0, {}});
    m_offsetStack.push_back(0);

    while (!m_offsetStack.empty())
    {
        std::uint16_t startOffset = m_offsetStack.back();
        m_offsetStack.pop_back();

        checkBasicBlock(m_code.getCode().drop_front(startOffset), startOffset, m_basicBlocks[startOffset]);
    }
}

ByteCodeTypeChecker::PossibleRetsMap ByteCodeTypeChecker::makeRetToMap() const
{
    PossibleRetsMap map;

    for (const auto& [returnAddr, subroutine] : m_returnAddressToSubroutineMap)
    {
        map[m_subroutineToReturnInfoMap.lookup(subroutine).retOffset].insert(returnAddr);
    }

    return map;
}
