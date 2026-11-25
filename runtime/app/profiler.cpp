// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The sampling profiler (bozo/app/profiler.h). The sampler's thread reads one instruction pointer per registered thread
// per tick into that thread's buffer, reserved when sampling starts: nothing is allocated while a thread is held (on
// Windows a suspended thread may hold the heap's lock, and the sampler would wait for it forever). The symbols are read
// once, when a profile is first named: the executable's COFF symbol table (MinGW keeps it) or ELF .symtab.
#include "bozo/app/profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define BOZO_PROFILER_WINDOWS 1
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <csignal>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <ucontext.h>
#include <unistd.h>
#define BOZO_PROFILER_LINUX 1
#endif

namespace bozo::app::profiler
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr size_t kSamplesPerThread = size_t(1) << 19; // 8.7 minutes at 1 kHz; then that thread stops counting

        struct Sampled
        {
            std::string name;
            bool alive = true;
#if BOZO_PROFILER_WINDOWS
            HANDLE handle = nullptr;
            DWORD id = 0;
#elif BOZO_PROFILER_LINUX
            pthread_t thread{};
#endif
            std::vector<uint64_t> pcs; // reserved at start: the sampler never grows it
        };

        std::mutex g_mutex; // the threads and their buffers
        std::vector<std::unique_ptr<Sampled>> g_threads;
        std::atomic<bool> g_running{false};
        std::thread g_sampler;
        Clock::time_point g_started{}, g_stopped{};

#if BOZO_PROFILER_LINUX
        // The SIGPROF handler's answer: the interrupted instruction pointer, then the flag the sampler waits for.
        std::atomic<uint64_t> g_signalPc{0};
        std::atomic<bool> g_signalDone{false};

        void onSignal(int, siginfo_t *, void *context)
        {
            const auto *uc = static_cast<const ucontext_t *>(context);
#if defined(__x86_64__)
            g_signalPc.store(static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]), std::memory_order_relaxed);
#else
            g_signalPc.store(static_cast<uint64_t>(uc->uc_mcontext.pc), std::memory_order_relaxed);
#endif
            g_signalDone.store(true, std::memory_order_release);
        }

        void installHandler()
        {
            static const bool installed = [] {
                struct sigaction sa{};
                sa.sa_sigaction = onSignal;
                sa.sa_flags = SA_SIGINFO | SA_RESTART;
                sigemptyset(&sa.sa_mask);
                return sigaction(SIGPROF, &sa, nullptr) == 0;
            }();
            (void)installed;
        }
#endif

        Sampled *current()
        {
            for (auto &t : g_threads)
            {
#if BOZO_PROFILER_WINDOWS
                if (t->alive && t->id == GetCurrentThreadId())
                    return t.get();
#elif BOZO_PROFILER_LINUX
                if (t->alive && pthread_equal(t->thread, pthread_self()))
                    return t.get();
#endif
            }
            return nullptr;
        }

        // One instruction pointer of a thread (0: none read).
        uint64_t sample(Sampled &t)
        {
#if BOZO_PROFILER_WINDOWS
            if (SuspendThread(t.handle) == static_cast<DWORD>(-1))
                return 0;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            uint64_t pc = 0;
            if (GetThreadContext(t.handle, &c))
#if defined(_M_ARM64) || defined(__aarch64__)
                pc = c.Pc;
#else
                pc = c.Rip;
#endif
            ResumeThread(t.handle);
            return pc;
#elif BOZO_PROFILER_LINUX
            g_signalDone.store(false, std::memory_order_relaxed);
            if (pthread_kill(t.thread, SIGPROF) != 0)
                return 0;
            const auto limit = Clock::now() + std::chrono::milliseconds(5);
            while (!g_signalDone.load(std::memory_order_acquire))
            {
                if (Clock::now() > limit)
                    return 0; // (a thread blocked with the signal masked: no sample)
                sched_yield();
            }
            return g_signalPc.load(std::memory_order_relaxed);
#else
            (void)t;
            return 0;
#endif
        }

        void samplerLoop()
        {
#if BOZO_PROFILER_WINDOWS
            // A high-resolution waitable timer: about a millisecond between ticks (Sleep(1) can be 15 ms).
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
            while (g_running.load(std::memory_order_relaxed))
            {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    for (auto &t : g_threads)
                        if (t->alive && t->pcs.size() < t->pcs.capacity())
                            if (const uint64_t pc = sample(*t))
                                t->pcs.push_back(pc);
                }
#if BOZO_PROFILER_WINDOWS
                LARGE_INTEGER due;
                due.QuadPart = -10000; // 1 ms, relative
                if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, INFINITE);
                else
                    Sleep(1);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
            }
#if BOZO_PROFILER_WINDOWS
            if (timer)
                CloseHandle(timer);
#endif
        }

        // ---- Symbols ----------------------------------------------------------------------------------------------

        struct Symbol
        {
            uint64_t address = 0, size = 0; // in the running program (its load address applied)
            std::string name;
        };

        struct Symbols
        {
            std::vector<Symbol> sorted;
            uint64_t begin = 0, end = 0; // the executable's image
        };

        std::string readable(const std::string &mangled)
        {
            std::string name = mangled;
#if defined(__GNUC__) || defined(__clang__)
            int status = 0;
            if (char *d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status))
            {
                if (status == 0)
                    name = d;
                std::free(d);
            }
#endif
            // Without the parameter list: the last top-level "(...)".
            if (!name.empty() && name.back() == ')')
            {
                int depth = 0;
                for (size_t i = name.size(); i-- > 0;)
                {
                    if (name[i] == ')')
                        ++depth;
                    else if (name[i] == '(' && --depth == 0)
                    {
                        if (i > 0)
                            name.resize(i);
                        break;
                    }
                }
            }
            return name;
        }

        std::vector<uint8_t> readFile(const std::filesystem::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        template <typename T> bool readAt(const std::vector<uint8_t> &file, uint64_t at, T &out)
        {
            if (at + sizeof(T) > file.size())
                return false;
            std::memcpy(&out, file.data() + at, sizeof(T));
            return true;
        }

#if BOZO_PROFILER_WINDOWS
        // The executable's COFF symbol table: its functions (type 0x20) in code sections, by their RVA.
        Symbols loadSymbols()
        {
            Symbols s;
            const auto module = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
            s.begin = reinterpret_cast<uint64_t>(module);
            s.end = s.begin + nt->OptionalHeader.SizeOfImage;
            wchar_t path[MAX_PATH * 4];
            const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
            if (!n)
                return s;
            const std::vector<uint8_t> file = readFile(std::filesystem::path(std::wstring(path, n)));
            IMAGE_DOS_HEADER fdos;
            IMAGE_FILE_HEADER header;
            if (!readAt(file, 0, fdos) || !readAt(file, static_cast<uint64_t>(fdos.e_lfanew) + 4, header) || !header.PointerToSymbolTable)
                return s;
            const uint64_t sections = static_cast<uint64_t>(fdos.e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) + header.SizeOfOptionalHeader;
            std::vector<IMAGE_SECTION_HEADER> section(header.NumberOfSections);
            for (size_t i = 0; i < section.size(); ++i)
                if (!readAt(file, sections + i * sizeof(IMAGE_SECTION_HEADER), section[i]))
                    return s;
            const uint64_t table = header.PointerToSymbolTable, strings = table + uint64_t(header.NumberOfSymbols) * 18u;
            for (uint32_t i = 0; i < header.NumberOfSymbols; ++i)
            {
                const uint64_t at = table + uint64_t(i) * 18u;
                if (at + 18 > file.size())
                    break;
                const uint8_t *r = file.data() + at;
                uint32_t value, zero, offset;
                int16_t sectionNumber;
                uint16_t type;
                std::memcpy(&zero, r, 4);
                std::memcpy(&offset, r + 4, 4);
                std::memcpy(&value, r + 8, 4);
                std::memcpy(&sectionNumber, r + 12, 2);
                std::memcpy(&type, r + 14, 2);
                const uint8_t aux = r[17];
                i += aux;
                if (sectionNumber <= 0 || static_cast<size_t>(sectionNumber) > section.size() || (type & 0xf0u) != 0x20u)
                    continue;
                const IMAGE_SECTION_HEADER &sec = section[static_cast<size_t>(sectionNumber - 1)];
                if (!(sec.Characteristics & IMAGE_SCN_CNT_CODE))
                    continue;
                std::string name;
                if (zero == 0)
                {
                    const uint64_t p = strings + offset;
                    if (p >= file.size())
                        continue;
                    const char *c = reinterpret_cast<const char *>(file.data() + p);
                    name.assign(c, strnlen(c, file.size() - p));
                }
                else
                    name.assign(reinterpret_cast<const char *>(r), strnlen(reinterpret_cast<const char *>(r), 8));
                s.sorted.push_back({s.begin + sec.VirtualAddress + value, 0, std::move(name)});
            }
            return s;
        }

        // A DLL's exported functions (its loaded export directory), by RVA: a sample in a system library named by the
        // export it falls after ("ntdll.dll!NtWaitForSingleObject": a thread waiting, not working).
        struct Exports
        {
            std::string module;
            std::vector<std::pair<uint32_t, std::string>> sorted;
        };

        const Exports &exportsOf(HMODULE m)
        {
            static std::map<HMODULE, Exports> cache;
            auto found = cache.find(m);
            if (found != cache.end())
                return found->second;
            Exports e;
            wchar_t path[MAX_PATH * 4];
            const DWORD n = GetModuleFileNameW(m, path, static_cast<DWORD>(std::size(path)));
            e.module = std::filesystem::path(std::wstring(path, n)).filename().string();
            const auto *base = reinterpret_cast<const uint8_t *>(m);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (dir.VirtualAddress && dir.Size)
            {
                const auto *ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
                const auto *functions = reinterpret_cast<const uint32_t *>(base + ex->AddressOfFunctions);
                const auto *names = reinterpret_cast<const uint32_t *>(base + ex->AddressOfNames);
                const auto *ordinals = reinterpret_cast<const uint16_t *>(base + ex->AddressOfNameOrdinals);
                for (DWORD i = 0; i < ex->NumberOfNames; ++i)
                {
                    const uint32_t rva = functions[ordinals[i]];
                    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
                        continue; // a forwarder, not code
                    e.sorted.emplace_back(rva, reinterpret_cast<const char *>(base + names[i]));
                }
                std::sort(e.sorted.begin(), e.sorted.end());
            }
            return cache.emplace(m, std::move(e)).first->second;
        }

        // A sample outside the executable: "module!export", or "[module]" when no export precedes it.
        std::string moduleOf(uint64_t pc)
        {
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(pc), &m))
                return "[unknown]";
            const Exports &e = exportsOf(m);
            const uint32_t rva = static_cast<uint32_t>(pc - reinterpret_cast<uint64_t>(m));
            auto it = std::upper_bound(e.sorted.begin(), e.sorted.end(), rva,
                                       [](uint32_t a, const std::pair<uint32_t, std::string> &b) { return a < b.first; });
            if (it == e.sorted.begin())
                return "[" + e.module + "]";
            return e.module + "!" + std::prev(it)->second;
        }
#elif BOZO_PROFILER_LINUX
        // The main program's ELF .symtab (else .dynsym): its functions, at the program's load address.
        Symbols loadSymbols()
        {
            Symbols s;
            struct Found
            {
                uint64_t base = 0, begin = ~uint64_t(0), end = 0;
            } found;
            dl_iterate_phdr(
                [](dl_phdr_info *info, size_t, void *data) -> int {
                    auto *f = static_cast<Found *>(data);
                    f->base = info->dlpi_addr;
                    for (int i = 0; i < info->dlpi_phnum; ++i)
                        if (info->dlpi_phdr[i].p_type == PT_LOAD)
                        {
                            f->begin = std::min<uint64_t>(f->begin, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
                            f->end = std::max<uint64_t>(f->end, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz);
                        }
                    return 1; // the first is the main program
                },
                &found);
            s.begin = found.begin;
            s.end = found.end;
            const std::vector<uint8_t> file = readFile("/proc/self/exe");
            Elf64_Ehdr eh;
            if (!readAt(file, 0, eh) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64)
                return s;
            std::vector<Elf64_Shdr> sh(eh.e_shnum);
            for (size_t i = 0; i < sh.size(); ++i)
                if (!readAt(file, eh.e_shoff + i * sizeof(Elf64_Shdr), sh[i]))
                    return s;
            const Elf64_Shdr *table = nullptr;
            for (const auto &h : sh)
                if (h.sh_type == SHT_SYMTAB)
                    table = &h;
            if (!table)
                for (const auto &h : sh)
                    if (h.sh_type == SHT_DYNSYM)
                        table = &h;
            if (!table || table->sh_link >= sh.size())
                return s;
            const Elf64_Shdr &strings = sh[table->sh_link];
            for (uint64_t at = table->sh_offset; at + sizeof(Elf64_Sym) <= table->sh_offset + table->sh_size; at += sizeof(Elf64_Sym))
            {
                Elf64_Sym sym;
                if (!readAt(file, at, sym) || ELF64_ST_TYPE(sym.st_info) != STT_FUNC || !sym.st_value)
                    continue;
                const uint64_t p = strings.sh_offset + sym.st_name;
                if (p >= file.size())
                    continue;
                const char *c = reinterpret_cast<const char *>(file.data() + p);
                s.sorted.push_back({found.base + sym.st_value, sym.st_size, std::string(c, strnlen(c, file.size() - p))});
            }
            return s;
        }

        std::string moduleOf(uint64_t pc)
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname)
            {
                const std::string module = std::filesystem::path(info.dli_fname).filename().string();
                if (info.dli_sname)
                    return module + "!" + readable(info.dli_sname);
                return "[" + module + "]";
            }
            struct Look
            {
                uint64_t pc;
                std::string name;
            } look{pc, "[unknown]"};
            dl_iterate_phdr(
                [](dl_phdr_info *info, size_t, void *data) -> int {
                    auto *l = static_cast<Look *>(data);
                    for (int i = 0; i < info->dlpi_phnum; ++i)
                    {
                        const auto &ph = info->dlpi_phdr[i];
                        const uint64_t b = info->dlpi_addr + ph.p_vaddr;
                        if (ph.p_type == PT_LOAD && l->pc >= b && l->pc < b + ph.p_memsz)
                        {
                            const std::string path = info->dlpi_name ? info->dlpi_name : "";
                            l->name = "[" + (path.empty() ? std::string("program") : std::filesystem::path(path).filename().string()) + "]";
                            return 1;
                        }
                    }
                    return 0;
                },
                &look);
            return look.name;
        }
#else
        Symbols loadSymbols() { return {}; }
        std::string moduleOf(uint64_t) { return "[unknown]"; }
#endif

        const Symbols &symbols()
        {
            static const Symbols s = [] {
                Symbols loaded = loadSymbols();
                std::sort(loaded.sorted.begin(), loaded.sorted.end(), [](const Symbol &a, const Symbol &b) { return a.address < b.address; });
                return loaded;
            }();
            return s;
        }

        std::string nameOf(uint64_t pc, std::unordered_map<std::string, std::string> &readableNames)
        {
            const Symbols &s = symbols();
            if (pc < s.begin || pc >= s.end)
                return moduleOf(pc);
            auto it = std::upper_bound(s.sorted.begin(), s.sorted.end(), pc, [](uint64_t a, const Symbol &b) { return a < b.address; });
            if (it == s.sorted.begin())
                return "[program]";
            --it;
            if (it->size && pc >= it->address + it->size)
                return "[program]";
            auto r = readableNames.find(it->name);
            if (r == readableNames.end())
                r = readableNames.emplace(it->name, readable(it->name)).first;
            return r->second;
        }
    } // namespace

    bool available()
    {
#if BOZO_PROFILER_WINDOWS || BOZO_PROFILER_LINUX
        return true;
#else
        return false;
#endif
    }

    void registerThread(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (Sampled *t = current())
        {
            t->name = name;
            return;
        }
        auto t = std::make_unique<Sampled>();
        t->name = name;
#if BOZO_PROFILER_WINDOWS
        t->id = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
#elif BOZO_PROFILER_LINUX
        installHandler();
        t->thread = pthread_self();
#endif
        if (g_running.load(std::memory_order_relaxed))
            t->pcs.reserve(kSamplesPerThread);
        g_threads.push_back(std::move(t));
    }

    void unregisterThread()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (Sampled *t = current())
        {
            t->alive = false;
#if BOZO_PROFILER_WINDOWS
            if (t->handle)
                CloseHandle(t->handle);
            t->handle = nullptr;
#endif
        }
    }

    void start()
    {
        if (!available() || g_running.exchange(true))
            return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // The ended threads' samples go with the last profile.
            g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(), [](const auto &t) { return !t->alive; }), g_threads.end());
            for (auto &t : g_threads)
            {
                t->pcs.clear();
                t->pcs.reserve(kSamplesPerThread);
            }
            g_started = Clock::now();
            g_stopped = {};
        }
        g_sampler = std::thread(samplerLoop);
    }

    void stop()
    {
        if (!g_running.exchange(false))
            return;
        if (g_sampler.joinable())
            g_sampler.join();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stopped = Clock::now();
    }

    bool running() { return g_running.load(std::memory_order_relaxed); }

    Profile profile(size_t top)
    {
        // The buffers copied under the lock, named outside it (reading the symbols takes a moment the first time).
        std::vector<std::pair<std::string, std::vector<uint64_t>>> copies;
        Profile p;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &t : g_threads)
                copies.emplace_back(t->name, t->pcs);
            if (g_started != Clock::time_point{})
                p.seconds = std::chrono::duration<double>((g_stopped == Clock::time_point{} ? Clock::now() : g_stopped) - g_started).count();
        }
        std::unordered_map<std::string, std::string> readableNames;
        std::unordered_map<uint64_t, std::string> names; // by address: a hot address is named once
        for (auto &[name, pcs] : copies)
        {
            Thread t;
            t.name = name;
            t.samples = pcs.size();
            std::unordered_map<std::string, uint64_t> counts;
            for (const uint64_t pc : pcs)
            {
                auto n = names.find(pc);
                if (n == names.end())
                    n = names.emplace(pc, nameOf(pc, readableNames)).first;
                ++counts[n->second];
            }
            for (auto &[function, count] : counts)
                t.functions.push_back({function, count});
            std::sort(t.functions.begin(), t.functions.end(), [](const Function &a, const Function &b) {
                return a.samples != b.samples ? a.samples > b.samples : a.name < b.name;
            });
            if (t.functions.size() > top)
                t.functions.resize(top);
            p.threads.push_back(std::move(t));
        }
        return p;
    }

    std::string text(const Profile &profile)
    {
        std::string out;
        char line[512];
        std::snprintf(line, sizeof line, "Profile: %.1f s sampled, about 1 ms between samples (self time: the function each sample fell in)\n",
                      profile.seconds);
        out += line;
        for (const Thread &t : profile.threads)
        {
            std::snprintf(line, sizeof line, "\nThread %s: %llu samples\n", t.name.c_str(), static_cast<unsigned long long>(t.samples));
            out += line;
            for (const Function &f : t.functions)
            {
                std::snprintf(line, sizeof line, "  %6.2f%% %8llu  %s\n", t.samples ? 100.0 * double(f.samples) / double(t.samples) : 0.0,
                              static_cast<unsigned long long>(f.samples), f.name.c_str());
                out += line;
            }
        }
        return out;
    }

    bool writeReport(const std::string &path, size_t top)
    {
        std::error_code ec;
        const std::filesystem::path p(path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        if (!out)
            return false;
        out << text(profile(top));
        return static_cast<bool>(out);
    }

    std::string reportPath(const std::string &folder)
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &local);
        return (std::filesystem::path(folder.empty() ? std::string(".") : folder) / ("profile_" + std::string(stamp) + ".txt")).string();
    }

    void frames(uint64_t ended, const std::string &folder)
    {
        struct Range
        {
            bool set = false, started = false, done = false;
            uint64_t from = 0, to = 0;
            std::string file;
        };
        static Range range = [] {
            Range r;
            const char *e = std::getenv("PS2X_PROFILE");
            if (!e || !*e)
                return r;
            char *end = nullptr;
            r.from = std::strtoull(e, &end, 10);
            if (end && *end == ':')
                r.to = std::strtoull(end + 1, &end, 10);
            if (end && *end == ':')
                r.file = end + 1;
            r.set = r.to > r.from;
            if (!r.set)
                std::fprintf(stderr, "[profiler] PS2X_PROFILE=%s: <from>:<to>[:<file>] frames expected\n", e);
            return r;
        }();
        if (!range.set || range.done)
            return;
        if (!range.started && ended >= range.from)
        {
            range.started = true;
            start();
            std::fprintf(stderr, "[profiler] sampling from frame %llu\n", static_cast<unsigned long long>(ended));
        }
        if (range.started && ended >= range.to)
        {
            range.done = true;
            stop();
            const std::string path = range.file.empty() ? reportPath(folder) : range.file;
            std::fprintf(stderr, "[profiler] frame %llu: report %s %s\n", static_cast<unsigned long long>(ended),
                         writeReport(path) ? "written to" : "could not be written to", path.c_str());
        }
    }
} // namespace bozo::app::profiler
