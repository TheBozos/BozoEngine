// SPDX-License-Identifier: MIT
#pragma once
#include "bozo/audio/libsd.h"
#include "bozo/libs/libraries.h"
#include "bozo/native/compiled_irx.h"
#include <memory>
#include <string>
#include <vector>
namespace bozo::native::irx
{
    struct Import
    {
        uint32_t address;
        std::string library;
        uint32_t ordinal;
    };
    struct Image
    {
        std::string name;
        uint64_t hash;
        uint32_t base, entry, gp;
        std::vector<uint8_t> bytes;
        std::vector<Import> imports, exports;
        bool nativeLibsd = false;
    };
    // Native host services for statically compiled sound libraries. Buffers and register values are ordinary
    // C++ state. Every executable target must have a linked function or a native SDK binding. There is no
    // instruction fetch/decode, CPU/device backend, firmware, interpreter or run-time code generation.
    class CompiledLibraries
    {
      public:
        explicit CompiledLibraries(libs::Libraries &libraries, uint32_t heapStart);
        ~CompiledLibraries();
        void addImage(Image image);
        int32_t startImage(const std::string &name);
        void reset();
        audio::Mixer *advanceTo(uint64_t eeCycle);
        void writeBuffer(uint32_t address, const uint8_t *data, uint32_t bytes);
        uint32_t allocateBuffer(uint32_t bytes);
        bool freeBuffer(uint32_t address);
        uint64_t nativeCalls() const;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace bozo::native::irx
