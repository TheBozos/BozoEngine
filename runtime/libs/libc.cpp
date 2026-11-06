// The C library calls TMB makes, answered natively on guest memory (libraries.md section 2).
#include "bozo/libs/libraries.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        // ---- memory and strings ----

        void memcpy_(Libraries &, Guest &g)
        {
            g.copy(g.arg(0), g.arg(1), g.arg(2));
            g.retU(g.arg(0));
        }

        void memset_(Libraries &, Guest &g)
        {
            g.fill(g.arg(0), static_cast<uint8_t>(g.arg(1)), g.arg(2));
            g.retU(g.arg(0));
        }

        void strlen_(Libraries &, Guest &g) { g.retU(g.stringLength(g.arg(0))); }

        void strchr_(Libraries &, Guest &g)
        {
            const uint32_t s = g.arg(0);
            const uint8_t c = static_cast<uint8_t>(g.arg(1));
            for (uint32_t i = 0;; ++i)
            {
                const uint8_t b = g.read8(s + i);
                if (b == c)
                {
                    g.retU(s + i);
                    return;
                }
                if (b == 0)
                {
                    g.ret(0);
                    return;
                }
            }
        }

        void strcpy_(Libraries &, Guest &g)
        {
            const uint32_t n = g.stringLength(g.arg(1));
            g.copy(g.arg(0), g.arg(1), n + 1);
            g.retU(g.arg(0));
        }

        // strcmp's result is -1, 0 or 1 (the reference's: never the difference of the bytes).
        void strcmp_(Libraries &, Guest &g)
        {
            const uint32_t a = g.arg(0), b = g.arg(1);
            for (uint32_t i = 0;; ++i)
            {
                const uint8_t x = g.read8(a + i), y = g.read8(b + i);
                if (x != y)
                {
                    g.ret(x < y ? -1 : 1);
                    return;
                }
                if (x == 0)
                {
                    g.ret(0);
                    return;
                }
            }
        }

        void strncat_(Libraries &, Guest &g)
        {
            const uint32_t d = g.arg(0), s = g.arg(1), n = g.arg(2);
            const uint32_t at = d + g.stringLength(d);
            const uint32_t k = g.stringLength(s, n);
            g.copy(at, s, k);
            g.write8(at + k, 0);
            g.retU(d);
        }

        // ---- formatted output ----

        void sprintf_(Libraries &, Guest &g)
        {
            const std::string s = formatGuest(g, g.arg(1), 2);
            g.write(g.arg(0), s.c_str(), static_cast<uint32_t>(s.size() + 1));
            g.ret(static_cast<int32_t>(s.size()));
        }

        void printf_(Libraries &l, Guest &g)
        {
            const std::string s = formatGuest(g, g.arg(0), 1);
            if (l.printOutput)
            {
                std::fwrite(s.data(), 1, s.size(), stdout);
                std::fflush(stdout);
            }
            g.ret(static_cast<int32_t>(s.size()));
        }

        // ---- the heap ----

        void malloc_(Libraries &l, Guest &g) { g.retU(l.gameHeap().allocate(g.arg(0))); }
        // Newlib's reentrant entry points share the same native allocator as malloc/free.
        void mallocR(Libraries &l, Guest &g) { g.retU(l.gameHeap().allocate(g.arg(1))); }
        void freeR(Libraries &l, Guest &g) { if(g.arg(1))l.gameHeap().release(g.arg(1)); }
        void callocR(Libraries &l, Guest &g) {
            const uint64_t bytes=uint64_t(g.arg(1))*g.arg(2);
            if(bytes>UINT32_MAX) {g.ret(0);return;}
            const uint32_t address=l.gameHeap().allocate(uint32_t(bytes));
            if(address && bytes)g.fill(address,0,uint32_t(bytes));
            g.retU(address);
        }


        void free_(Libraries &l, Guest &g)
        {
            if (g.arg(0))
                l.gameHeap().release(g.arg(0));
        }

        // ---- rand ----

        void srand_(Libraries &l, Guest &g) { l.rand.seed(g.arg(0)); }
        void rand_(Libraries &l, Guest &g) { g.ret(l.rand.next()); }
    }

    std::string formatGuest(Guest &g, uint32_t format, int first)
    {
        const std::string f = g.string(format);
        std::string out;
        int slot = first;
        for (size_t i = 0; i < f.size(); ++i)
        {
            if (f[i] != '%')
            {
                out += f[i];
                continue;
            }
            // %[flags][width][.precision][length]conversion
            std::string spec = "%";
            size_t j = i + 1;
            while (j < f.size() && std::strchr("-+ #0", f[j]))
                spec += f[j++];
            if (j < f.size() && f[j] == '*')
            {
                spec += std::to_string(static_cast<int32_t>(g.arg(slot++)));
                ++j;
            }
            while (j < f.size() && f[j] >= '0' && f[j] <= '9')
                spec += f[j++];
            if (j < f.size() && f[j] == '.')
            {
                spec += f[j++];
                if (j < f.size() && f[j] == '*')
                {
                    spec += std::to_string(static_cast<int32_t>(g.arg(slot++)));
                    ++j;
                }
                while (j < f.size() && f[j] >= '0' && f[j] <= '9')
                    spec += f[j++];
            }
            int longs = 0;
            while (j < f.size() && (f[j] == 'l' || f[j] == 'h' || f[j] == 'q' || f[j] == 'L'))
            {
                if (f[j] == 'l' || f[j] == 'q' || f[j] == 'L')
                    ++longs;
                ++j;
            }
            if (j >= f.size())
            {
                out += f.substr(i);
                break;
            }
            const char conv = f[j];
            char buf[512];
            int n = 0;
            switch (conv)
            {
            case '%': out += '%'; break;
            case 'd':
            case 'i':
            {
                const uint64_t v = g.arg64(slot++);
                const long long x = longs ? static_cast<long long>(v) : static_cast<long long>(static_cast<int32_t>(v));
                n = std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), x);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                const uint64_t v = g.arg64(slot++);
                const unsigned long long x = longs ? v : static_cast<uint32_t>(v);
                n = std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), x);
                break;
            }
            case 'c': n = std::snprintf(buf, sizeof(buf), (spec + 'c').c_str(), static_cast<int>(static_cast<uint8_t>(g.arg(slot++)))); break;
            // %p as the reference's host printed it: 16 hex digits, no 0x (match4p #16615141: "toc starts at
            // 0000000000496c00", 70 characters).
            case 'p': n = std::snprintf(buf, sizeof(buf), "%016x", g.arg(slot++)); break;
            case 's':
            {
                const uint32_t p = g.arg(slot++);
                const std::string s = p ? g.string(p) : std::string("(null)");
                n = std::snprintf(buf, sizeof(buf), (spec + 's').c_str(), s.c_str());
                if (n >= static_cast<int>(sizeof(buf)))
                {
                    out += s; // long strings: no width to apply in TMB's formats
                    n = 0;
                }
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            {
                // A double in one 64-bit slot (the EE's 64-bit registers).
                const uint64_t bits = g.arg64(slot++);
                double d;
                std::memcpy(&d, &bits, 8);
                n = std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
                break;
            }
            default: out += spec + conv; break;
            }
            if (n > 0)
                out.append(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
            i = j;
        }
        return out;
    }

    void addLibc(std::vector<Function> &out)
    {
        out.push_back({"memcpy", kLibc, memcpy_});
        out.push_back({"memset", kLibc, memset_});
        out.push_back({"strlen", kLibc, strlen_});
        out.push_back({"strchr", kLibc, strchr_});
        out.push_back({"strcpy", kLibc, strcpy_});
        out.push_back({"strcmp", kLibc, strcmp_});
        out.push_back({"strncat", kLibc, strncat_});
        out.push_back({"sprintf", kLibc, sprintf_});
        out.push_back({"printf", kLibc, printf_});
        out.push_back({"scePrintf", kLibc, printf_});
        out.push_back({"_malloc_r", kLibc, mallocR});
        out.push_back({"_free_r", kLibc, freeR});
        out.push_back({"_calloc_r", kLibc, callocR});
        out.push_back({"malloc", kLibc, malloc_});
        out.push_back({"free", kLibc, free_});
        out.push_back({"srand", kLibc, srand_});
        out.push_back({"rand", kLibc, rand_});
    }
}
