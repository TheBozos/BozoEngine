// libcdvd (libraries.md section 8): the disc image's sectors, the stream, the drive's time. What TMB calls: sceCdInit,
// sceCdMmode, sceCdDiskReady, sceFsReset, sceCdReadClock, sceCdRead, sceCdSync, sceCdGetError, sceCdStInit,
// sceCdStStart, sceCdStRead, sceCdStStop.
#include "bozo/libs/libraries.h"

#include "bozo/ee/dispatcher.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>

namespace bozo::libs
{
    // ---- DiscImage -----------------------------------------------------------------------------------------------

    namespace
    {
        bool seek(std::FILE *f, uint64_t at)
        {
#ifdef _WIN32
            return _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0;
#else
            return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
        }
    }

    DiscImage::~DiscImage()
    {
        if (f_)
            std::fclose(f_);
    }

    bool DiscImage::open(const std::string &path, std::string &error)
    {
        if (f_)
            std::fclose(f_);
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_)
        {
            error = "cannot open the disc image " + path;
            return false;
        }
        path_ = path;
        uint64_t size = 0;
#ifdef _WIN32
        _fseeki64(f_, 0, SEEK_END);
        size = static_cast<uint64_t>(_ftelli64(f_));
#else
        fseeko(f_, 0, SEEK_END);
        size = static_cast<uint64_t>(ftello(f_));
#endif
        sectors_ = static_cast<uint32_t>(size / kSectorBytes);
        // An ISO 9660 image has its primary volume descriptor at sector 16 ("\1CD001").
        uint8_t pvd[kSectorBytes];
        if (!read(16, 1, pvd) || pvd[0] != 1 || std::string(reinterpret_cast<char *>(pvd + 1), 5) != "CD001")
        {
            error = path + " is not an ISO 9660 image (no primary volume descriptor at sector 16)";
            std::fclose(f_);
            f_ = nullptr;
            return false;
        }
        return true;
    }

    bool DiscImage::read(uint32_t lbn, uint32_t count, uint8_t *out)
    {
        if (!f_)
            return false;
        std::memset(out, 0, static_cast<size_t>(count) * kSectorBytes);
        if (lbn >= sectors_)
            return true;
        const uint32_t n = std::min(count, sectors_ - lbn);
        if (!seek(f_, static_cast<uint64_t>(lbn) * kSectorBytes))
            return false;
        return std::fread(out, kSectorBytes, n, f_) == n;
    }

    // ---- The library ---------------------------------------------------------------------------------------------

    namespace
    {
        // Copies `count` sectors from `lbn` to guest memory at `to`. False when the disc cannot be read.
        bool sectorsTo(Libraries &l, Guest &g, uint32_t lbn, uint32_t count, uint32_t to)
        {
            CdState &cd = l.cd;
            if (!cd.disc.isOpen())
            {
                std::string err;
                if (cd.imagePath.empty() || !cd.disc.open(cd.imagePath, err))
                    return false;
            }
            std::vector<uint8_t> data(static_cast<size_t>(count) * kSectorBytes);
            if (!cd.disc.read(lbn, count, data.data()))
                return false;
            g.write(to, data.data(), static_cast<uint32_t>(data.size()));
            return true;
        }

        uint8_t bcd(int v) { return static_cast<uint8_t>((v / 10) * 16 + v % 10); }

        // sceCdInit(mode), sceCdMmode(media): 1. sceCdDiskReady(mode): 2 (ready). sceFsReset(): 0.
        void init(Libraries &, Guest &g) { g.ret(1); }
        void mmode(Libraries &, Guest &g) { g.ret(1); }
        void diskReady(Libraries &, Guest &g) { g.ret(2); }
        void fsReset(Libraries &, Guest &g) { g.ret(0); }

        // sceCdReadClock(sceCdCLOCK *): stat, second, minute, hour, pad, day, month, year (BCD); 1.
        void readClock(Libraries &l, Guest &g)
        {
            int sec = 0, min = 0, hour = 12, day = 18, month = 6, year = 1; // the reference's deterministic clock
            if (l.cd.hostClock)
            {
                const std::time_t t = std::time(nullptr);
                if (const std::tm *tm = std::localtime(&t))
                {
                    sec = tm->tm_sec;
                    min = tm->tm_min;
                    hour = tm->tm_hour;
                    day = tm->tm_mday;
                    month = tm->tm_mon + 1;
                    year = tm->tm_year % 100;
                }
            }
            const uint8_t c[8] = {0, bcd(sec), bcd(min), bcd(hour), 0, bcd(day), bcd(month), bcd(year)};
            g.write(g.arg(0), c, 8);
            g.ret(1);
        }

        // sceCdRead(lbn, sectors, buffer, mode): the sectors are in the buffer when it returns, after a drive's time;
        // 1 (0 when the disc cannot be read).
        void read(Libraries &l, Guest &g)
        {
            const uint32_t lbn = g.arg(0), n = g.arg(1), to = g.arg(2);
            CdState &cd = l.cd;
            const bool ok = sectorsTo(l, g, lbn, n, to);
            cd.error = ok ? 0 : -1;
            g.cycles = l.callCycles(static_cast<uint64_t>(n) * kSectorCycles);
            cd.readEnd = l.now(g) + g.cycles;
            g.ret(ok ? 1 : 0);
        }

        // sceCdSync(mode): mode 0 waits for the read to finish (a vblank wait, its v0 0), then 0; mode 1: 1 while a
        // read is in progress, else 0.
        void sync(Libraries &l, Guest &g)
        {
            CdState &cd = l.cd;
            const uint64_t now = l.now(g);
            const bool busy = now < cd.readEnd;
            if (g.arg(0) != 0)
            {
                g.ret(busy ? 1 : 0);
                return;
            }
            uint64_t token = 0;
            g.cycles = l.callCycles(0);
            if (l.waitPoint(busy, l.vblankTokenFor(cd.readEnd), token))
            {
                l.waitVblank(token, 0);
                return;
            }
            g.ret(0);
        }

        // sceCdGetError(): the last read's error (0: none).
        void getError(Libraries &l, Guest &g) { g.ret(l.cd.error); }

        // sceCdStInit(ring sectors, banks, IOP buffer): 1.
        void stInit(Libraries &l, Guest &g)
        {
            CdState &cd = l.cd;
            cd.ringSectors = g.arg(0);
            cd.banks = g.arg(1);
            cd.iopBuffer = g.arg(2);
            g.ret(1);
        }

        // sceCdStStart(lbn, mode *): the stream starts at lbn; 1.
        void stStart(Libraries &l, Guest &g)
        {
            CdState &cd = l.cd;
            cd.streaming = true;
            cd.streamLbn = g.arg(0);
            cd.delivered = 0;
            cd.produced = 0;
            cd.producedAt = l.now(g);
            g.ret(1);
        }

        // sceCdStStop(): 1.
        void stStop(Libraries &l, Guest &g)
        {
            l.cd.streaming = false;
            g.ret(1);
        }

        // The sectors of a stream read go to the game: the next `n` of the stream.
        void deliver(Libraries &l, Guest &g, uint32_t n, uint32_t to, uint32_t errorAt)
        {
            CdState &cd = l.cd;
            const bool ok = sectorsTo(l, g, cd.streamLbn + cd.delivered, n, to);
            cd.delivered += n;
            if (errorAt)
                g.write32(errorAt, ok ? 0 : 1);
            g.ret(static_cast<int32_t>(n));
        }

        // sceCdStRead(sectors, buffer, mode, error *): the next sectors of the stream. Those that have arrived are
        // copied at once; mode 1 (TMB's) waits for the rest: a vblank wait (its result the vblank's parity) finished
        // as the thread resumes, by their copy and v0 = sectors. Returns the sectors copied (mode 0: those there).
        void stRead(Libraries &l, Guest &g)
        {
            CdState &cd = l.cd;
            const uint32_t n = g.arg(0), to = g.arg(1), mode = g.arg(2), errorAt = g.arg(3);
            cd.advance(l.now(g));
            uint64_t available = std::min<uint64_t>(n, cd.produced - cd.delivered);
            if (l.pin.active)
            {
                // The ring's state when the reference made the call: what it copied at once.
                const int64_t base = GuestMemory::flatOffset(to, 1);
                available = 0;
                if (base >= 0 && l.pin.writtenEnd > base)
                    available = std::min<uint64_t>(n, (l.pin.writtenEnd - static_cast<uint64_t>(base) + kSectorBytes - 1) / kSectorBytes);
            }
            g.cycles = l.callCycles(0);
            uint64_t token = 0;
            if (l.waitPoint(mode == 1 && available < n, l.vblankTokenFor(cd.readyAt(n)), token))
            {
                const uint32_t now = static_cast<uint32_t>(available);
                if (now)
                    sectorsTo(l, g, cd.streamLbn + cd.delivered, now, to);
                cd.delivered += now;
                cd.pendingSectors = n - now;
                cd.pendingBuffer = to + now * kSectorBytes;
                cd.pendingError = errorAt;
                l.waitVblank(token, -1, [n](Libraries &l2, Guest &g2) {
                    CdState &c = l2.cd;
                    c.advance(l2.now(g2));
                    deliver(l2, g2, c.pendingSectors, c.pendingBuffer, c.pendingError);
                    c.pendingSectors = 0;
                    g2.ret(static_cast<int32_t>(n));
                });
                return;
            }
            deliver(l, g, mode == 1 ? n : static_cast<uint32_t>(available), to, errorAt);
        }
    }

    void addCd(std::vector<Function> &out)
    {
        out.push_back({"sceCdInit", kCd, init});
        out.push_back({"sceCdMmode", kCd, mmode});
        out.push_back({"sceCdDiskReady", kCd, diskReady});
        out.push_back({"sceFsReset", kCd, fsReset});
        out.push_back({"sceCdReadClock", kCd, readClock});
        out.push_back({"sceCdRead", kCd, read, false, true});
        out.push_back({"sceCdSync", kCd, sync, false, true});
        out.push_back({"sceCdGetError", kCd, getError});
        out.push_back({"sceCdStInit", kCd, stInit});
        out.push_back({"sceCdStStart", kCd, stStart});
        out.push_back({"sceCdStRead", kCd, stRead, false, true});
        out.push_back({"sceCdStStop", kCd, stStop});
    }
}
