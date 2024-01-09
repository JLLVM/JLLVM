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

#pragma once

#include <cassert>
#include <cstdint>
#include <memory>

#include "Executor.hpp"
#include "JavaFrame.hpp"

namespace jllvm
{

constexpr auto deleteOsrFrame = [](const std::uint64_t* osrFrame) { delete[] osrFrame; };

class OSRState;

/// Class representing 'Executor's capable of producing OSR versions of methods for entry via OSR.
class OSRTarget : public Executor
{
public:
    /// Returns an OSR version for 'method' starting at the given 'byteCodeOffset'.
    /// The method must have the signature '<original-ret-type>(uint64_t*)' where the 'uint64_t*' is the buffer the
    /// 'OSRState's are initialized with by the 'createOSRState*' methods below. This buffer should be used to
    /// initialize the abstract machine state at the given 'byteCodeOffset'.
    virtual void* getOSREntry(const Method& method, std::uint16_t byteCodeOffset) = 0;

    /// Methods creating an 'OSRState' method suitable for use by functions returned by 'getOSREntry' and initializing
    /// it from their given parameters.

    virtual OSRState createOSRStateFromInterpreterFrame(InterpreterFrame frame) = 0;

    virtual OSRState createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                                       Throwable* throwable) = 0;
};

/// Class representing the abstract machine state required for transitioning execution from one tier to another.
/// It is used to internally build up a buffer in the OSR calling convention to initialize the abstract machine state.
class OSRState
{
    std::unique_ptr<std::uint64_t[]> m_buffer;
    OSRTarget* m_target{};
    std::uint16_t m_byteCodeOffset{};

public:
    /// Constructor used by 'OSRTarget's to initialize the OSR state as required by their OSR entries.
    OSRState(OSRTarget& target, std::uint16_t byteCodeOffset, std::unique_ptr<std::uint64_t[]> internalCCStructure)
        : m_target(&target), m_byteCodeOffset(byteCodeOffset), m_buffer(std::move(internalCCStructure))
    {
    }

    /// Releases the internal buffer filled with the OSR state and returns it.
    ///
    /// The pointed to array depends on the target being OSRed into.
    /// This array is used by OSR versions to initialize their machine state.
    std::uint64_t* release()
    {
        assert(m_buffer && "must not have been released previously");
        return m_buffer.release();
    }

    /// Returns the bytecode offset with which this instance was initialized.
    std::uint16_t getByteCodeOffset() const
    {
        return m_byteCodeOffset;
    }

    /// Returns the OSR target of this state.
    OSRTarget& getTarget() const
    {
        return *m_target;
    }
};
} // namespace jllvm
