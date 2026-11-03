// The native libraries' table, guest access and shared state. See bozo/libs/libraries.h and docs/specs/libraries.md.
#include "bozo/libs/libraries.h"

#include "bozo/ee/dispatcher.h"

#include <algorithm>

namespace bozo::libs
{
    // ---- Guest ---------------------------------------------------------------------------------------------------

    uint64_t Guest::arg64(int i) const
    {
        if (i < 4)
            return ctx_->r[4 + i].ud[0];
        if (i < 8)
            return ctx_->r[8 + i - 4].ud[0];
        // Past the eight argument registers: 64-bit slots from the caller's sp.
        const uint32_t at = ctx_->r[29].uw[0] + 8u * static_cast<uint32_t>(i - 8);
        uint64_t v;
        std::memcpy(&v, in(at, 8), 8);
        return v;
    }

    const uint8_t *Guest::in(uint32_t address, uint32_t bytes) const
    {
        const int64_t o = GuestMemory::flatOffset(address, bytes ? bytes : 1);
        if (o < 0)
            throw BadAccess{address, bytes};
        return rt_.memory.flat() + o;
    }

    uint16_t Guest::read16(uint32_t address) const
    {
        uint16_t v;
        std::memcpy(&v, in(address, 2), 2);
        return v;
    }

    uint32_t Guest::read32(uint32_t address) const
    {
        uint32_t v;
        std::memcpy(&v, in(address, 4), 4);
        return v;
    }

    uint32_t Guest::stringLength(uint32_t address, uint32_t limit) const
    {
        uint32_t n = 0;
        while (n < limit && read8(address + n) != 0)
            ++n;
        return n;
    }

    std::string Guest::string(uint32_t address, uint32_t limit) const
    {
        const uint32_t n = stringLength(address, limit);
        return n ? std::string(reinterpret_cast<const char *>(in(address, n)), n) : std::string();
    }

    uint8_t *Guest::out(uint32_t address, uint32_t bytes)
    {
        const int64_t o = GuestMemory::flatOffset(address, bytes ? bytes : 1);
        if (o < 0)
            throw BadAccess{address, bytes};
        uint8_t *p = rt_.memory.flat() + o;
        if (journal_ && bytes)
        {
            journal_->entries.push_back({static_cast<uint32_t>(o), bytes, journal_->old.size()});
            journal_->old.insert(journal_->old.end(), p, p + bytes);
        }
        return p;
    }

    void Guest::write(uint32_t address, const void *data, uint32_t bytes)
    {
        if (bytes)
            std::memmove(out(address, bytes), data, bytes);
    }

    void Guest::fill(uint32_t address, uint8_t value, uint32_t bytes)
    {
        if (bytes)
            std::memset(out(address, bytes), value, bytes);
    }

    void Guest::copy(uint32_t to, uint32_t from, uint32_t bytes)
    {
        if (!bytes)
            return;
        const uint8_t *src = in(from, bytes);
        std::memmove(out(to, bytes), src, bytes);
    }

    // ---- Heap ----------------------------------------------------------------------------------------------------

    void Heap::reset(uint32_t start, uint32_t end)
    {
        free_.clear();
        used_.clear();
        end_ = end;
        if (end > start)
            free_[start] = end - start;
    }

    uint32_t Heap::allocate(uint32_t bytes)
    {
        uint32_t n = (bytes + 15u) & ~15u;
        if (n == 0)
            n = 16;
        for (auto it = free_.begin(); it != free_.end(); ++it)
            if (it->second >= n)
            {
                const uint32_t a = it->first, left = it->second - n;
                free_.erase(it);
                if (left)
                    free_[a + n] = left;
                used_[a] = n;
                return a;
            }
        return 0;
    }

    bool Heap::release(uint32_t address)
    {
        auto u = used_.find(address);
        if (u == used_.end())
            return false;
        uint32_t a = address, n = u->second;
        used_.erase(u);
        auto next = free_.lower_bound(a);
        if (next != free_.end() && next->first == a + n)
        {
            n += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == a)
            {
                prev->second += n;
                return true;
            }
        }
        free_[a] = n;
        return true;
    }

    // ---- Libraries -----------------------------------------------------------------------------------------------

    const char *libraryName(Library l)
    {
        switch (l)
        {
        case kLibc: return "libc";
        case kLibm: return "libm";
        case kGs: return "gs";
        case kPad: return "pad";
        case kMc: return "mc";
        default: return "?";
        }
    }

    bool parseLibraries(const std::string &list, uint32_t &mask)
    {
        mask = 0;
        size_t at = 0;
        while (at <= list.size())
        {
            const size_t comma = list.find(',', at);
            const std::string n = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (n == "all")
                mask |= kAllLibraries;
            else if (n == "none" || n.empty())
                ;
            else
            {
                bool known = false;
                for (Library l : {kLibc, kLibm, kGs, kPad, kMc})
                    if (n == libraryName(l))
                    {
                        mask |= l;
                        known = true;
                    }
                if (!known)
                    return false;
            }
            if (comma == std::string::npos)
                break;
            at = comma + 1;
        }
        return true;
    }

    Libraries::Libraries()
    {
        addLibc(functions_);
        addLibm(functions_);
        addGs(functions_);
        addPad(functions_);
        addMc(functions_);
        for (size_t i = 0; i < functions_.size(); ++i)
            byName_[functions_[i].name] = i;
    }

    const Function *Libraries::find(const std::string &name) const
    {
        std::string n = name;
        if (n.rfind("ps2_", 0) == 0)
            n = n.substr(4);
        auto it = byName_.find(n);
        return it == byName_.end() ? nullptr : &functions_[it->second];
    }

    Heap &Libraries::gameHeap()
    {
        if (!heap.ready())
        {
            // The reference's heap runs from SetupHeap's result to the end of RAM (nothing in TMB's runs reaches it).
            const uint32_t start = d_ ? d_->heapStart() : 0;
            heap.reset(start, 0x2000000u);
        }
        return heap;
    }

    bool Libraries::observeTraced(const std::string &name, const uint32_t a[4], uint32_t v0, std::string &why)
    {
        if (name == "rand")
        {
            const uint32_t mine = static_cast<uint32_t>(rand.next());
            if (mine != v0)
            {
                why = "rand (inside an answered call) returned " + std::to_string(v0) + ", the native generator " + std::to_string(mine);
                return false;
            }
        }
        else if (name == "srand")
            rand.seed(a[0]);
        else if (name == "malloc")
        {
            const uint32_t mine = gameHeap().allocate(a[0]);
            if (mine != v0)
            {
                why = "malloc (inside an answered call) returned " + std::to_string(v0) + ", the native heap " + std::to_string(mine);
                return false;
            }
        }
        else if (name == "free" && a[0])
            gameHeap().release(a[0]);
        return true;
    }

    bool Libraries::observeAnswered(const std::string &name, Guest &g, std::string &why)
    {
        // The reference's libmpeg takes 0x600 bytes from the game's heap at each sceMpegCreate and never gives them
        // back; it keeps the block's address at work area + 0x44 (libraries.md section 2.2).
        if (name == "sceMpegCreate")
        {
            const uint32_t mine = gameHeap().allocate(0x600);
            const uint32_t theirs = g.read32(g.arg(1) + 0x44);
            if (mine != theirs)
            {
                why = "sceMpegCreate's block is at " + std::to_string(theirs) + ", the native heap gives " + std::to_string(mine);
                return false;
            }
        }
        return true;
    }
}
