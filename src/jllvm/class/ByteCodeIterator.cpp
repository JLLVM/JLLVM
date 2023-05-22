#include "ByteCodeIterator.hpp"

#include <llvm/Support/Alignment.h>
#include <llvm/Support/Endian.h>

#include <jllvm/support/Bytes.hpp>

std::optional<jllvm::SingletonOp> consumeSingleton(llvm::ArrayRef<char> bytes, std::size_t currentOffset)
{
    auto opCode = static_cast<jllvm::SingletonOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::SingletonOpCodes::AALoad:
        case jllvm::SingletonOpCodes::AAStore:
        case jllvm::SingletonOpCodes::AConstNull:
        case jllvm::SingletonOpCodes::ALoad0:
        case jllvm::SingletonOpCodes::ALoad1:
        case jllvm::SingletonOpCodes::ALoad2:
        case jllvm::SingletonOpCodes::ALoad3:
        case jllvm::SingletonOpCodes::AReturn:
        case jllvm::SingletonOpCodes::ArrayLength:
        case jllvm::SingletonOpCodes::AStore0:
        case jllvm::SingletonOpCodes::AStore1:
        case jllvm::SingletonOpCodes::AStore2:
        case jllvm::SingletonOpCodes::AStore3:
        case jllvm::SingletonOpCodes::AThrow:
        case jllvm::SingletonOpCodes::BALoad:
        case jllvm::SingletonOpCodes::BAStore:
        case jllvm::SingletonOpCodes::CALoad:
        case jllvm::SingletonOpCodes::CAStore:
        case jllvm::SingletonOpCodes::D2F:
        case jllvm::SingletonOpCodes::D2I:
        case jllvm::SingletonOpCodes::D2L:
        case jllvm::SingletonOpCodes::DAdd:
        case jllvm::SingletonOpCodes::DALoad:
        case jllvm::SingletonOpCodes::DAStore:
        case jllvm::SingletonOpCodes::DCmpG:
        case jllvm::SingletonOpCodes::DCmpL:
        case jllvm::SingletonOpCodes::DConst0:
        case jllvm::SingletonOpCodes::DConst1:
        case jllvm::SingletonOpCodes::DDiv:
        case jllvm::SingletonOpCodes::DLoad0:
        case jllvm::SingletonOpCodes::DLoad1:
        case jllvm::SingletonOpCodes::DLoad2:
        case jllvm::SingletonOpCodes::DLoad3:
        case jllvm::SingletonOpCodes::DMul:
        case jllvm::SingletonOpCodes::DNeg:
        case jllvm::SingletonOpCodes::DRem:
        case jllvm::SingletonOpCodes::DReturn:
        case jllvm::SingletonOpCodes::DStore0:
        case jllvm::SingletonOpCodes::DStore1:
        case jllvm::SingletonOpCodes::DStore2:
        case jllvm::SingletonOpCodes::DStore3:
        case jllvm::SingletonOpCodes::DSub:
        case jllvm::SingletonOpCodes::Dup:
        case jllvm::SingletonOpCodes::DupX1:
        case jllvm::SingletonOpCodes::DupX2:
        case jllvm::SingletonOpCodes::Dup2:
        case jllvm::SingletonOpCodes::Dup2X1:
        case jllvm::SingletonOpCodes::Dup2X2:
        case jllvm::SingletonOpCodes::F2D:
        case jllvm::SingletonOpCodes::F2I:
        case jllvm::SingletonOpCodes::F2L:
        case jllvm::SingletonOpCodes::FAdd:
        case jllvm::SingletonOpCodes::FALoad:
        case jllvm::SingletonOpCodes::FAStore:
        case jllvm::SingletonOpCodes::FCmpG:
        case jllvm::SingletonOpCodes::FCmpL:
        case jllvm::SingletonOpCodes::FConst0:
        case jllvm::SingletonOpCodes::FConst1:
        case jllvm::SingletonOpCodes::FConst2:
        case jllvm::SingletonOpCodes::FDiv:
        case jllvm::SingletonOpCodes::FLoad0:
        case jllvm::SingletonOpCodes::FLoad1:
        case jllvm::SingletonOpCodes::FLoad2:
        case jllvm::SingletonOpCodes::FLoad3:
        case jllvm::SingletonOpCodes::FMul:
        case jllvm::SingletonOpCodes::FNeg:
        case jllvm::SingletonOpCodes::FRem:
        case jllvm::SingletonOpCodes::FReturn:
        case jllvm::SingletonOpCodes::FStore0:
        case jllvm::SingletonOpCodes::FStore1:
        case jllvm::SingletonOpCodes::FStore2:
        case jllvm::SingletonOpCodes::FStore3:
        case jllvm::SingletonOpCodes::FSub:
        case jllvm::SingletonOpCodes::I2B:
        case jllvm::SingletonOpCodes::I2C:
        case jllvm::SingletonOpCodes::I2D:
        case jllvm::SingletonOpCodes::I2F:
        case jllvm::SingletonOpCodes::I2L:
        case jllvm::SingletonOpCodes::I2S:
        case jllvm::SingletonOpCodes::IAdd:
        case jllvm::SingletonOpCodes::IALoad:
        case jllvm::SingletonOpCodes::IAnd:
        case jllvm::SingletonOpCodes::IAStore:
        case jllvm::SingletonOpCodes::IConstM1:
        case jllvm::SingletonOpCodes::IConst0:
        case jllvm::SingletonOpCodes::IConst1:
        case jllvm::SingletonOpCodes::IConst2:
        case jllvm::SingletonOpCodes::IConst3:
        case jllvm::SingletonOpCodes::IConst4:
        case jllvm::SingletonOpCodes::IConst5:
        case jllvm::SingletonOpCodes::IDiv:
        case jllvm::SingletonOpCodes::ILoad0:
        case jllvm::SingletonOpCodes::ILoad1:
        case jllvm::SingletonOpCodes::ILoad2:
        case jllvm::SingletonOpCodes::ILoad3:
        case jllvm::SingletonOpCodes::IMul:
        case jllvm::SingletonOpCodes::INeg:
        case jllvm::SingletonOpCodes::IOr:
        case jllvm::SingletonOpCodes::IRem:
        case jllvm::SingletonOpCodes::IReturn:
        case jllvm::SingletonOpCodes::IShl:
        case jllvm::SingletonOpCodes::IShr:
        case jllvm::SingletonOpCodes::IStore0:
        case jllvm::SingletonOpCodes::IStore1:
        case jllvm::SingletonOpCodes::IStore2:
        case jllvm::SingletonOpCodes::IStore3:
        case jllvm::SingletonOpCodes::ISub:
        case jllvm::SingletonOpCodes::IUShr:
        case jllvm::SingletonOpCodes::IXor:
        case jllvm::SingletonOpCodes::L2D:
        case jllvm::SingletonOpCodes::L2F:
        case jllvm::SingletonOpCodes::L2I:
        case jllvm::SingletonOpCodes::LAdd:
        case jllvm::SingletonOpCodes::LALoad:
        case jllvm::SingletonOpCodes::LAnd:
        case jllvm::SingletonOpCodes::LAStore:
        case jllvm::SingletonOpCodes::LCmp:
        case jllvm::SingletonOpCodes::LConst0:
        case jllvm::SingletonOpCodes::LConst1:
        case jllvm::SingletonOpCodes::LDiv:
        case jllvm::SingletonOpCodes::LLoad0:
        case jllvm::SingletonOpCodes::LLoad1:
        case jllvm::SingletonOpCodes::LLoad2:
        case jllvm::SingletonOpCodes::LLoad3:
        case jllvm::SingletonOpCodes::LMul:
        case jllvm::SingletonOpCodes::LNeg:
        case jllvm::SingletonOpCodes::LOr:
        case jllvm::SingletonOpCodes::LRem:
        case jllvm::SingletonOpCodes::LReturn:
        case jllvm::SingletonOpCodes::LShl:
        case jllvm::SingletonOpCodes::LShr:
        case jllvm::SingletonOpCodes::LStore0:
        case jllvm::SingletonOpCodes::LStore1:
        case jllvm::SingletonOpCodes::LStore2:
        case jllvm::SingletonOpCodes::LStore3:
        case jllvm::SingletonOpCodes::LSub:
        case jllvm::SingletonOpCodes::LUShr:
        case jllvm::SingletonOpCodes::LXor:
        case jllvm::SingletonOpCodes::MonitorEnter:
        case jllvm::SingletonOpCodes::MonitorExit:
        case jllvm::SingletonOpCodes::Nop:
        case jllvm::SingletonOpCodes::Pop:
        case jllvm::SingletonOpCodes::Pop2:
        case jllvm::SingletonOpCodes::Return:
        case jllvm::SingletonOpCodes::SALoad:
        case jllvm::SingletonOpCodes::SAStore:
        case jllvm::SingletonOpCodes::Swap:
        {
            return jllvm::SingletonOp{jllvm::consume<jllvm::SingletonOpCodes>(bytes), currentOffset};
        }
    }
}

std::optional<jllvm::LocalIndexedOp> consumeLocalIndex(llvm::ArrayRef<char> bytes, std::size_t currentOffset)
{
    auto opCode = static_cast<jllvm::LocalIndexedOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::LocalIndexedOpCodes::ALoad:
        case jllvm::LocalIndexedOpCodes::AStore:
        case jllvm::LocalIndexedOpCodes::DLoad:
        case jllvm::LocalIndexedOpCodes::DStore:
        case jllvm::LocalIndexedOpCodes::FLoad:
        case jllvm::LocalIndexedOpCodes::FStore:
        case jllvm::LocalIndexedOpCodes::ILoad:
        case jllvm::LocalIndexedOpCodes::IStore:
        case jllvm::LocalIndexedOpCodes::LLoad:
        case jllvm::LocalIndexedOpCodes::LStore:
        case jllvm::LocalIndexedOpCodes::Ret:
        {
            return jllvm::LocalIndexedOp{jllvm::consume<jllvm::LocalIndexedOpCodes>(bytes),
                                         jllvm::consume<std::uint8_t>(bytes), currentOffset};
        };
    }
}

std::optional<jllvm::PoolIndexedOp> consumePoolIndex(llvm::ArrayRef<char> bytes, std::size_t currentOffset)
{
    auto opCode = static_cast<jllvm::PoolIndexedOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::PoolIndexedOpCodes::LDC:
        {
            return jllvm::PoolIndexedOp{jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes),
                                        jllvm::consume<std::uint8_t>(bytes), currentOffset};
        }
        case jllvm::PoolIndexedOpCodes::ANewArray:
        case jllvm::PoolIndexedOpCodes::CheckCast:
        case jllvm::PoolIndexedOpCodes::GetField:
        case jllvm::PoolIndexedOpCodes::GetStatic:
        case jllvm::PoolIndexedOpCodes::InstanceOf:
        case jllvm::PoolIndexedOpCodes::InvokeSpecial:
        case jllvm::PoolIndexedOpCodes::InvokeStatic:
        case jllvm::PoolIndexedOpCodes::InvokeVirtual:
        case jllvm::PoolIndexedOpCodes::LDCW:
        case jllvm::PoolIndexedOpCodes::LDC2W:
        case jllvm::PoolIndexedOpCodes::New:
        case jllvm::PoolIndexedOpCodes::PutField:
        case jllvm::PoolIndexedOpCodes::PutStatic:
        {
            return jllvm::PoolIndexedOp{jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes),
                                        jllvm::consume<std::uint16_t>(bytes), currentOffset};
        }
        case jllvm::PoolIndexedOpCodes::InvokeDynamic:
        case jllvm::PoolIndexedOpCodes::InvokeInterface:
        {
            opCode = jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes);
            auto index = jllvm::consume<std::uint16_t>(bytes);
            auto count = jllvm::consume<std::uint8_t>(bytes);
            auto padding = jllvm::consume<std::uint8_t>(bytes);
            if (opCode == jllvm::PoolIndexedOpCodes::InvokeDynamic)
            {
                assert(count == 0);
            }
            else
            {
                assert(count != 0);
            }
            assert(padding == 0);
            return jllvm::PoolIndexedOp{opCode, index, currentOffset};
        }
    }
}

std::optional<jllvm::BranchOffsetOp> consumeBranchOffset(llvm::ArrayRef<char> bytes, std::size_t currentOffset)
{
    auto opCode = static_cast<jllvm::BranchOffsetOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::BranchOffsetOpCodes::Goto:
        case jllvm::BranchOffsetOpCodes::IfACmpEq:
        case jllvm::BranchOffsetOpCodes::IfACmpNe:
        case jllvm::BranchOffsetOpCodes::IfICmpEq:
        case jllvm::BranchOffsetOpCodes::IfICmpNe:
        case jllvm::BranchOffsetOpCodes::IfICmpLt:
        case jllvm::BranchOffsetOpCodes::IfICmpGe:
        case jllvm::BranchOffsetOpCodes::IfICmpGt:
        case jllvm::BranchOffsetOpCodes::IfICmpLe:
        case jllvm::BranchOffsetOpCodes::IfEq:
        case jllvm::BranchOffsetOpCodes::IfNe:
        case jllvm::BranchOffsetOpCodes::IfLt:
        case jllvm::BranchOffsetOpCodes::IfGe:
        case jllvm::BranchOffsetOpCodes::IfGt:
        case jllvm::BranchOffsetOpCodes::IfLe:
        case jllvm::BranchOffsetOpCodes::IfNonNull:
        case jllvm::BranchOffsetOpCodes::IfNull:
        case jllvm::BranchOffsetOpCodes::JSR:
        {
            return jllvm::BranchOffsetOp{jllvm::consume<jllvm::BranchOffsetOpCodes>(bytes),
                                         jllvm::consume<std::int16_t>(bytes), currentOffset};
        }
        case jllvm::BranchOffsetOpCodes::GotoW:
        case jllvm::BranchOffsetOpCodes::JSRw:
        {
            return jllvm::BranchOffsetOp{jllvm::consume<jllvm::BranchOffsetOpCodes>(bytes),
                                         jllvm::consume<std::int32_t>(bytes), currentOffset};
        }
    }
}

jllvm::ByteCodeOp jllvm::ByteCodeIterator::currentOp() const
{
    if (auto singleton = consumeSingleton(m_current, m_offset))
    {
        return singleton.value();
    }

    if (auto localIndex = consumeLocalIndex(m_current, m_offset))
    {
        return localIndex.value();
    }

    if (auto poolIndex = consumePoolIndex(m_current, m_offset))
    {
        return poolIndex.value();
    }

    if (auto branchOffset = consumeBranchOffset(m_current, m_offset))
    {
        return branchOffset.value();
    }

    auto copy = m_current;

    switch (consume<SpecialOpCodes>(copy))
    {
        default: llvm_unreachable("Unknown opcode");
        case SpecialOpCodes::BIPush:
        {
            return BIPushOp{consume<std::int8_t>(copy), m_offset};
        }
        case SpecialOpCodes::NewArray:
        {
            return NewArrayOp{consume<NewArrayOp::ArrayType>(copy), m_offset};
        }
        case SpecialOpCodes::IInc:
        {
            return IIncOp{consume<std::uint8_t>(copy), consume<std::int8_t>(copy), m_offset};
        }
        case SpecialOpCodes::SIPush:
        {
            return SIPushOp{consume<std::int16_t>(copy), m_offset};
        }
        case SpecialOpCodes::LookupSwitch:
        case SpecialOpCodes::MultiANewArray:
        case SpecialOpCodes::TableSwitch:
        case SpecialOpCodes::Wide:
        {
            llvm_unreachable("NOT YET IMPLEMENTED");
        }
    }
}

std::size_t jllvm::ByteCodeIterator::currentOpSize() const
{
    switch (static_cast<OpCodes>(m_current.front()))
    {
        default: llvm_unreachable("Unknown opcode");
        case OpCodes::AALoad:
        case OpCodes::AAStore:
        case OpCodes::AConstNull:
        case OpCodes::ALoad0:
        case OpCodes::ALoad1:
        case OpCodes::ALoad2:
        case OpCodes::ALoad3:
        case OpCodes::AReturn:
        case OpCodes::ArrayLength:
        case OpCodes::AStore0:
        case OpCodes::AStore1:
        case OpCodes::AStore2:
        case OpCodes::AStore3:
        case OpCodes::AThrow:
        case OpCodes::BALoad:
        case OpCodes::BAStore:
        case OpCodes::CALoad:
        case OpCodes::CAStore:
        case OpCodes::D2F:
        case OpCodes::D2I:
        case OpCodes::D2L:
        case OpCodes::DAdd:
        case OpCodes::DALoad:
        case OpCodes::DAStore:
        case OpCodes::DCmpG:
        case OpCodes::DCmpL:
        case OpCodes::DConst0:
        case OpCodes::DConst1:
        case OpCodes::DDiv:
        case OpCodes::DLoad0:
        case OpCodes::DLoad1:
        case OpCodes::DLoad2:
        case OpCodes::DLoad3:
        case OpCodes::DMul:
        case OpCodes::DNeg:
        case OpCodes::DRem:
        case OpCodes::DReturn:
        case OpCodes::DStore0:
        case OpCodes::DStore1:
        case OpCodes::DStore2:
        case OpCodes::DStore3:
        case OpCodes::DSub:
        case OpCodes::Dup:
        case OpCodes::DupX1:
        case OpCodes::DupX2:
        case OpCodes::Dup2:
        case OpCodes::Dup2X1:
        case OpCodes::Dup2X2:
        case OpCodes::F2D:
        case OpCodes::F2I:
        case OpCodes::F2L:
        case OpCodes::FAdd:
        case OpCodes::FALoad:
        case OpCodes::FAStore:
        case OpCodes::FCmpG:
        case OpCodes::FCmpL:
        case OpCodes::FConst0:
        case OpCodes::FConst1:
        case OpCodes::FConst2:
        case OpCodes::FDiv:
        case OpCodes::FLoad0:
        case OpCodes::FLoad1:
        case OpCodes::FLoad2:
        case OpCodes::FLoad3:
        case OpCodes::FMul:
        case OpCodes::FNeg:
        case OpCodes::FRem:
        case OpCodes::FReturn:
        case OpCodes::FStore0:
        case OpCodes::FStore1:
        case OpCodes::FStore2:
        case OpCodes::FStore3:
        case OpCodes::FSub:
        case OpCodes::I2B:
        case OpCodes::I2C:
        case OpCodes::I2D:
        case OpCodes::I2F:
        case OpCodes::I2L:
        case OpCodes::I2S:
        case OpCodes::IAdd:
        case OpCodes::IALoad:
        case OpCodes::IAnd:
        case OpCodes::IAStore:
        case OpCodes::IConstM1:
        case OpCodes::IConst0:
        case OpCodes::IConst1:
        case OpCodes::IConst2:
        case OpCodes::IConst3:
        case OpCodes::IConst4:
        case OpCodes::IConst5:
        case OpCodes::IDiv:
        case OpCodes::ILoad0:
        case OpCodes::ILoad1:
        case OpCodes::ILoad2:
        case OpCodes::ILoad3:
        case OpCodes::IMul:
        case OpCodes::INeg:
        case OpCodes::IOr:
        case OpCodes::IRem:
        case OpCodes::IReturn:
        case OpCodes::IShl:
        case OpCodes::IShr:
        case OpCodes::IStore0:
        case OpCodes::IStore1:
        case OpCodes::IStore2:
        case OpCodes::IStore3:
        case OpCodes::ISub:
        case OpCodes::IUShr:
        case OpCodes::IXor:
        case OpCodes::L2D:
        case OpCodes::L2F:
        case OpCodes::L2I:
        case OpCodes::LAdd:
        case OpCodes::LALoad:
        case OpCodes::LAnd:
        case OpCodes::LAStore:
        case OpCodes::LCmp:
        case OpCodes::LConst0:
        case OpCodes::LConst1:
        case OpCodes::LDiv:
        case OpCodes::LLoad0:
        case OpCodes::LLoad1:
        case OpCodes::LLoad2:
        case OpCodes::LLoad3:
        case OpCodes::LMul:
        case OpCodes::LNeg:
        case OpCodes::LOr:
        case OpCodes::LRem:
        case OpCodes::LReturn:
        case OpCodes::LShl:
        case OpCodes::LShr:
        case OpCodes::LStore0:
        case OpCodes::LStore1:
        case OpCodes::LStore2:
        case OpCodes::LStore3:
        case OpCodes::LSub:
        case OpCodes::LUShr:
        case OpCodes::LXor:
        case OpCodes::MonitorEnter:
        case OpCodes::MonitorExit:
        case OpCodes::Nop:
        case OpCodes::Pop:
        case OpCodes::Pop2:
        case OpCodes::Return:
        case OpCodes::SALoad:
        case OpCodes::SAStore:
        case OpCodes::Swap: return 1;

        case OpCodes::ALoad:
        case OpCodes::AStore:
        case OpCodes::BIPush:
        case OpCodes::DLoad:
        case OpCodes::DStore:
        case OpCodes::FLoad:
        case OpCodes::FStore:
        case OpCodes::ILoad:
        case OpCodes::IStore:
        case OpCodes::LDC:
        case OpCodes::LLoad:
        case OpCodes::LStore:
        case OpCodes::NewArray:
        case OpCodes::Ret: return 2;

        case OpCodes::ANewArray:
        case OpCodes::CheckCast:
        case OpCodes::GetField:
        case OpCodes::GetStatic:
        case OpCodes::Goto:
        case OpCodes::IfACmpEq:
        case OpCodes::IfACmpNe:
        case OpCodes::IfICmpEq:
        case OpCodes::IfICmpNe:
        case OpCodes::IfICmpLt:
        case OpCodes::IfICmpGe:
        case OpCodes::IfICmpGt:
        case OpCodes::IfICmpLe:
        case OpCodes::IfEq:
        case OpCodes::IfNe:
        case OpCodes::IfLt:
        case OpCodes::IfGe:
        case OpCodes::IfGt:
        case OpCodes::IfLe:
        case OpCodes::IfNonNull:
        case OpCodes::IfNull:
        case OpCodes::IInc:
        case OpCodes::InstanceOf:
        case OpCodes::InvokeSpecial:
        case OpCodes::InvokeStatic:
        case OpCodes::InvokeVirtual:
        case OpCodes::JSR:
        case OpCodes::LDCW:
        case OpCodes::LDC2W:
        case OpCodes::New:
        case OpCodes::PutField:
        case OpCodes::PutStatic:
        case OpCodes::SIPush: return 3;

        case OpCodes::GotoW:
        case OpCodes::InvokeDynamic:
        case OpCodes::InvokeInterface:
        case OpCodes::JSRw: return 5;

        case OpCodes::LookupSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current.drop_front().data(), llvm::Align(4));
            auto pairCountPtr = m_current.drop_front(5 + padding);
            auto pairCount = consume<std::uint32_t>(pairCountPtr);
            return 1 + padding + 4 + 8 * pairCount;
        }

        case OpCodes::MultiANewArray: return 4;

        case OpCodes::TableSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current.drop_front().data(), llvm::Align(4));
            auto padded = m_current.drop_front(5 + padding);
            auto lowByte = consume<std::uint32_t>(padded);
            auto highByte = consume<std::uint32_t>(padded);

            return 1 + padding + 4 + 4 + 4 + (highByte - lowByte + 1) * 4;
        }
        case OpCodes::Wide: return static_cast<OpCodes>(m_current[1]) == OpCodes::IInc ? 6 : 4;
    }
}
