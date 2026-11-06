// libcdvd (libraries.md section 8): the disc image's sectors, the stream, the drive's time. What TMB calls: sceCdInit,
// sceCdMmode, sceCdDiskReady, sceFsReset, sceCdReadClock, sceCdRead, sceCdSync, sceCdGetError, sceCdStInit,
// sceCdStStart, sceCdStRead, sceCdStStop.
#include "bozo/libs/libraries.h"

#include "bozo/ee/dispatcher.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>
#include <cctype>

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

    bool DiscImage::layer1Start(uint32_t &start)
    {
        uint8_t pvd[kSectorBytes];
        if (!read(16, 1, pvd))
            return false;
        const uint32_t layer0 = uint32_t(pvd[80]) | uint32_t(pvd[81]) << 8 | uint32_t(pvd[82]) << 16 | uint32_t(pvd[83]) << 24;
        if (layer0 <= 16 || layer0 >= sectors_ || !read(layer0, 1, pvd) || pvd[0] != 1 ||
            std::string(reinterpret_cast<char *>(pvd + 1), 5) != "CD001")
            return false;
        start = layer0 - 16;
        return true;
    }

    bool DiscImage::findFile(const std::string &path, uint32_t &lbn, uint32_t &bytes, uint32_t layer)
    {
        const auto word = [](const uint8_t *p) {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        };
        const auto normal = [](std::string value) {
            const size_t version = value.find(';');
            if (version != std::string::npos) value.resize(version);
            for (char &c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return value;
        };
        std::string clean = path;
        for (char &c : clean) if (c == '\\') c = '/';
        if (const auto colon = clean.find(':'); colon != std::string::npos) clean.erase(0, colon + 1);
        std::vector<std::string> parts;
        for (size_t at = 0; at < clean.size();) {
            const size_t end = clean.find('/', at);
            const auto part = clean.substr(at, end == std::string::npos ? end : end - at);
            if (!part.empty() && part != ".") parts.push_back(normal(part));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        if (parts.empty()) return false;
        uint32_t base = 0;
        if (layer > 1 || (layer == 1 && !layer1Start(base))) return false;
        uint8_t pvd[kSectorBytes];
        if (!read(base + 16, 1, pvd) || pvd[156] < 34) return false;
        uint32_t sector = base + word(pvd + 158), size = word(pvd + 166);
        for (size_t part = 0; part < parts.size(); ++part) {
            if (!size || size > 8u * 1024 * 1024) return false;
            const uint32_t count = (size + kSectorBytes - 1) / kSectorBytes;
            if (sector >= sectors_ || count > sectors_ - sector) return false;
            std::vector<uint8_t> directory(size_t(count) * kSectorBytes);
            if (!read(sector, count, directory.data())) return false;
            bool found = false;
            for (size_t at = 0; at < size;) {
                const uint32_t length = directory[at];
                if (!length) { at = (at / kSectorBytes + 1) * kSectorBytes; continue; }
                if (length < 34 || at + length > size) return false;
                const uint8_t *record = directory.data() + at;
                if (33u + record[32] > length) return false;
                const std::string name(reinterpret_cast<const char *>(record + 33), record[32]);
                if (normal(name) == parts[part]) {
                    if (part + 1 < parts.size() && !(record[25] & 2)) return false;
                    sector = base + word(record + 2); size = word(record + 10); found = true; break;
                }
                at += length;
            }
            if (!found) return false;
        }
        lbn = sector; bytes = size;
        return true;
    }

    bool discRead(Libraries &l, uint32_t lbn, uint32_t count, uint8_t *out)
    {
        CdState &cd = l.cd;
        if (!cd.disc.isOpen())
        {
            std::string err;
            if (cd.imagePath.empty() || !cd.disc.open(cd.imagePath, err))
                return false;
        }
        return cd.disc.read(lbn, count, out);
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
        void init(Libraries &l, Guest &g) { l.cd.initialized=g.arg(0)!=2;l.cd.error=0;g.ret(1); }
        void status(Libraries &l, Guest &g)
        {
            // Public SDK status values from libcdvd-common.h. These describe
            // the native file job, not an addressable console device bank.
            const auto &cd=l.cd;
            g.ret(!cd.initialized?0:cd.error?0x20:(cd.streaming || l.now(g)<cd.readEnd)?6:0x0a);
        }
        void readPosition(Libraries &l, Guest &g)
        {
            // The SDK reports a byte offset only while the read command is
            // active. Our native read copies the complete job before returning;
            // completion clears its observable position, not the last sector.
            g.retU(l.now(g)<l.cd.readEnd?l.cd.readBytes:0);
        }
        void mmode(Libraries &, Guest &g) { g.ret(1); }
        void diskReady(Libraries &, Guest &g) { g.ret(2); }
        void fsReset(Libraries &, Guest &g) { g.ret(0); }

        void callback(Libraries &l, Guest &g)
        {
            // The SDK refuses replacement while a read is in progress.
            if(l.now(g)<l.cd.readEnd) { g.ret(0); return; }
            const auto old=l.cd.callback;
            l.cd.callback=g.arg(0);l.cd.callbackGp=g.context()->r[28].uw[0];
            g.ret(static_cast<int32_t>(old));
        }

        void initCallback(Libraries &l, Guest &g)
        {
            if(l.cd.callbackConfigured) { g.ret(0); return; }
            // sceCdInitEeCB(priority, stack, stack_bytes): native completion uses
            // the handler queue. Keep its caller-supplied stack, without an EE
            // SDK worker waiting on private semaphore globals.
            const uint32_t stack=g.arg(1),bytes=g.arg(2);
            if(g.arg(0)>=ee::kPriorities || !bytes || stack>0xffffffffu-bytes ||
               !g.runtime().memory.hostPointer(stack,bytes)) { g.ret(-1); return; }
            l.cd.callbackStack=stack+bytes;l.cd.callbackConfigured=true;g.ret(1);
        }

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
            cd.readBytes=ok?n*kSectorBytes:0;
            g.cycles = l.callCycles(static_cast<uint64_t>(n) * kSectorCycles);
            cd.readEnd = l.now(g) + g.cycles;
            if(ok && cd.callback)
            {
                if(!l.dispatcher()) throw std::runtime_error("CD callback requires native dispatcher");
                ee::QueuedHandler h;h.kind=ee::kInvokeLibraryCall;h.pc=cd.callback;
                h.gp=cd.callbackGp;h.sp=cd.callbackStack;h.a[0]=1; // SCECdFuncRead
                // The existing native read returns after charging its full
                // read duration; this callback cannot execute inside that call.
                l.queueHandler(h,{});
            }
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

    namespace {
        void searchLayer(Libraries &l, Guest &g, uint32_t layer)
        {
            // sceCdlFILE: sector, byte length, name[16], date[8].
            uint8_t sector[kSectorBytes];
            if (!discRead(l, 16, 1, sector)) { g.ret(0); return; }
            const std::string name = g.string(g.arg(1));
            uint32_t lbn = 0, bytes = 0;
            if (!l.cd.disc.findFile(name, lbn, bytes, layer)) { g.ret(0); return; }
            uint8_t result[32] = {};
            std::memcpy(result, &lbn, 4); std::memcpy(result + 4, &bytes, 4);
            const auto slash = name.find_last_of("/\\");
            const auto leaf = name.substr(slash == std::string::npos ? 0 : slash + 1);
            std::memcpy(result + 8, leaf.data(), std::min<size_t>(16, leaf.size()));
            g.write(g.arg(0), result, sizeof(result)); g.ret(1);
        }
        void searchFile(Libraries &l, Guest &g) { searchLayer(l, g, 0); }
        // sceCdLayerSearchFile(file, name, layer).
        void layerSearchFile(Libraries &l, Guest &g) { searchLayer(l, g, g.arg(2)); }
        // sceCdReadDvdDualInfo(int *onDual, u_int *layer1Start).
        void readDvdDualInfo(Libraries &l, Guest &g)
        {
            uint8_t sector[kSectorBytes];
            uint32_t start = 0;
            const int32_t dual = discRead(l, 16, 1, sector) && l.cd.disc.layer1Start(start) ? 1 : 0;
            if (!dual)
                start = 0;
            g.write(g.arg(0), &dual, sizeof(dual));
            g.write(g.arg(1), &start, sizeof(start));
            g.ret(1);
        }
    }

    namespace {
        void fsInit(Libraries &, Guest &g) { g.ret(0); }
        void fsOpen(Libraries &l, Guest &g) {
            uint8_t sector[kSectorBytes];
            if ((g.arg(1) & 3u) != 1u || !discRead(l, 16, 1, sector)) { g.ret(-1); return; }
            uint32_t lbn=0, bytes=0;
            if (!l.cd.disc.findFile(g.string(g.arg(0)),lbn,bytes)) { g.ret(-1); return; }
            const int32_t id=l.cd.nextFile++; l.cd.files.emplace(id,CdState::File{lbn,bytes,0}); g.ret(id);
        }
        void fsClose(Libraries &l, Guest &g) { g.ret(l.cd.files.erase(g.sarg(0)) ? 0 : -1); }
        void fsSeek(Libraries &l, Guest &g) {
            auto it=l.cd.files.find(g.sarg(0));
            if (it==l.cd.files.end() || g.arg(2)>2) { g.ret(-1); return; }
            auto &f=it->second;
            const int64_t base=g.arg(2)==0 ? 0 : g.arg(2)==1 ? f.position : f.bytes;
            const int64_t pos=base+g.sarg(1);
            if (pos<0 || pos>INT32_MAX) { g.ret(-1); return; }
            f.position=static_cast<uint32_t>(pos); g.ret(static_cast<int32_t>(pos));
        }
        void fsRead(Libraries &l, Guest &g) {
            auto it=l.cd.files.find(g.sarg(0));
            if (it==l.cd.files.end() || g.sarg(2)<0) { g.ret(-1); return; }
            auto &f=it->second;
            const uint32_t bytes=std::min(g.arg(2),f.position<f.bytes ? f.bytes-f.position : 0);
            if (!bytes) { g.ret(0); return; }
            const uint32_t skip=f.position%kSectorBytes;
            const uint32_t count=(skip+bytes+kSectorBytes-1)/kSectorBytes;
            std::vector<uint8_t> data(size_t(count)*kSectorBytes);
            if (!discRead(l,f.sector+f.position/kSectorBytes,count,data.data())) { g.ret(-1); return; }
            g.write(g.arg(1),data.data()+skip,bytes); f.position+=bytes; g.ret(static_cast<int32_t>(bytes));
        }
        void fsWrite(Libraries &, Guest &g) {
            if (g.arg(0)!=1 && g.arg(0)!=2) { g.ret(-1); return; }
            const auto *data=g.in(g.arg(1),g.arg(2));
            g.ret(static_cast<int32_t>(std::fwrite(data,1,g.arg(2),g.arg(0)==1 ? stdout : stderr)));
        }
    }

    void addCd(std::vector<Function> &out)
    {
        out.push_back({"sceFsInit", kCd, fsInit});
        out.push_back({"sceOpen", kCd, fsOpen});
        out.push_back({"sceClose", kCd, fsClose});
        out.push_back({"sceRead", kCd, fsRead});
        out.push_back({"sceWrite", kCd, fsWrite});
        out.push_back({"sceLseek", kCd, fsSeek});
        out.push_back({"sceCdSearchFile", kCd, searchFile});
        out.push_back({"sceCdLayerSearchFile", kCd, layerSearchFile});
        out.push_back({"sceCdReadDvdDualInfo", kCd, readDvdDualInfo});
        out.push_back({"sceCdInit", kCd, init});
        out.push_back({"sceCdStatus", kCd, status});
        out.push_back({"sceCdGetReadPos", kCd, readPosition});
        out.push_back({"sceCdCallback", kCd, callback});
        out.push_back({"sceCdInitEeCB", kCd, initCallback});
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
