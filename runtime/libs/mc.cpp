// libmc (libraries.md section 7): two formatted PS2 memory cards, backed by host files or memory. Every operation
// finishes in its call; sceMcSync hands its result over, as the reference's did.
#include "bozo/libs/libraries.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace bozo::libs
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kCardClusters = 0x2000; // 8 MB in 1 KB clusters: what an empty card reports free
        constexpr uint16_t kAttrDirectory = 0x80a3;  // as the reference reports "." and ".."
        constexpr uint16_t kAttrFile = 0x8497;       // read, write, execute, closed, file, exists (not in the logs)

        std::string normalize(const std::string &name)
        {
            std::string n = name;
            std::replace(n.begin(), n.end(), '\\', '/');
            while (!n.empty() && n.front() == '/')
                n.erase(n.begin());
            while (!n.empty() && n.back() == '/')
                n.pop_back();
            return n;
        }

        std::string resolve(McState &state,int port,const std::string &path) {
            std::string input=path;std::replace(input.begin(),input.end(),'\\','/');
            const bool absolute=!input.empty() && input.front()=='/';
            if(!absolute && port>=0 && port<2 && !state.currentDirectory[port].empty())
                input=state.currentDirectory[port]+"/"+input;
            std::vector<std::string> parts;
            for(size_t at=0;at<input.size();) {
                const auto end=input.find('/',at);const auto part=input.substr(at,end==std::string::npos?end:end-at);
                if(part=="..") {if(!parts.empty())parts.pop_back();}
                else if(!part.empty() && part!=".")parts.push_back(part);
                if(end==std::string::npos)break;at=end+1;
            }
            std::string out;for(const auto &part:parts){if(!out.empty())out+='/';out+=part;}return out;
        }

        std::string parentOf(const std::string &path)
        {
            const size_t slash = path.rfind('/');
            return slash == std::string::npos ? std::string() : path.substr(0, slash);
        }

        std::string baseOf(const std::string &path)
        {
            const size_t slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        // A name pattern with '*' and '?' (libmc's GetDir).
        bool matches(const char *p, const char *s)
        {
            if (*p == 0)
                return *s == 0;
            if (*p == '*')
                return matches(p + 1, s) || (*s && matches(p, s + 1));
            if (*s && (*p == '?' || *p == *s))
                return matches(p + 1, s + 1);
            return false;
        }

        void putDate(uint8_t *d, const McDate &t)
        {
            d[0] = 0;
            d[1] = t.second;
            d[2] = t.minute;
            d[3] = t.hour;
            d[4] = t.day;
            d[5] = t.month;
            d[6] = static_cast<uint8_t>(t.year);
            d[7] = static_cast<uint8_t>(t.year >> 8);
        }

        // One sceMcTblGetDir entry (64 bytes).
        void putEntry(Guest &g, uint32_t at, const std::string &name, const McCard::Entry &e)
        {
            uint8_t d[64] = {};
            putDate(d + 0, e.created);
            putDate(d + 8, e.modified);
            const uint32_t size = e.directory ? 0 : static_cast<uint32_t>(e.data.size());
            std::memcpy(d + 16, &size, 4);
            std::memcpy(d + 20, &e.attributes, 2);
            std::memcpy(d + 32, name.c_str(), std::min<size_t>(name.size(), 31));
            g.write(at, d, 64);
        }

        void finish(Libraries &l, int32_t function, int32_t result)
        {
            if(std::getenv("BOZO_TRACE_MC"))std::fprintf(stderr,"[mc] complete function=%d result=%d\n",function,result);
            l.mc.pending = true;
            l.mc.function = function;
            l.mc.result = result;
        }

        bool validSlot(const Libraries &l,uint32_t slot) {
            return slot<32 && (l.mc.slotMask & (1u<<slot));
        }

        bool ready(Libraries &l, int port)
        {
            if (port < 0 || port > 1)
                return false;
            if (!l.mc.formatted)
            {
                for (int p = 0; p < 2; ++p)
                    l.mc.cards[static_cast<size_t>(p)].format(
                        l.mc.hostDirectory.empty() ? std::string() : l.mc.hostDirectory + "/mc" + std::to_string(p), l.mc.date);
                l.mc.formatted = true;
            }
            return true;
        }

        // libmc's function numbers (what sceMcSync's cmd would report).
        enum : int32_t
        {
            kFnGetInfo = 1, kFnOpen = 2, kFnClose = 3, kFnRead = 5, kFnWrite = 6, kFnMkdir = 0xB, kFnChdir = 0xC, kFnGetDir = 0xD,
            kFnSeek = 4, kFnFlush = 0xA, kFnSetFileInfo = 0xE, kFnDelete = 0xF, kFnFormat = 0x10, kFnUnformat = 0x11,
            kFnEntSpace = 0x12, kFnRename = 0x13, kFnChangePriority = 0x14,
        };

        void init(Libraries &l, Guest &g)
        {
            l.mc.known = {};
            g.ret(0);
        }

        // sceMcGetInfo(port, slot, type, free, format): a PS2 card (2), its free clusters, formatted (1), written at
        // once; the result (sceMcSync) is -1 at the first call on a port after sceMcInit (a new card), 0 after.
        void getInfo(Libraries &l, Guest &g)
        {
            const int port = static_cast<int>(g.arg(0));
            if (!ready(l, port) || !validSlot(l,g.arg(1)))
            {
                // A multitap slot (slot 1-3: no multitap on the card ports): no card there, its type, free clusters and
                // format 0, the result -4 (match4p #228107-#228109: TMB looks at slots 1-3 once its pads' multitap is
                // open).
                const bool slot = !validSlot(l,g.arg(1));
                if (slot)
                    for (uint32_t k = 2; k <= 4; ++k)
                        if (g.arg(k))
                            g.write32(g.arg(k), 0);
                finish(l, kFnGetInfo, slot ? -4 : -10);
                g.ret(0);
                return;
            }
            if (g.arg(2))
                g.write32(g.arg(2), 2);
            if (g.arg(3))
                g.write32(g.arg(3), l.mc.card(port).freeClusters());
            if (g.arg(4))
                g.write32(g.arg(4), l.mc.unformatted[static_cast<size_t>(port)] ? 0 : 1);
            if (std::getenv("BOZO_TRACE_MC")) std::fprintf(stderr,"[mc] info port=%d slot=%u free=%u out=%x,%x,%x\n",port,g.arg(1),l.mc.card(port).freeClusters(),g.arg(2),g.arg(3),g.arg(4));
            finish(l, kFnGetInfo, l.mc.known[static_cast<size_t>(port)] ? 0 : -1);
            l.mc.known[static_cast<size_t>(port)] = true;
            g.ret(0);
        }

        // sceMcGetDir(port, slot, name, mode, maxent, table): the entries matching `name` written at once (a listing
        // with a wildcard starts with "." and ".."); the result is their count, or -4 when there is none.
        void getDir(Libraries &l, Guest &g)
        {
            const int port = static_cast<int>(g.arg(0));
            const std::string name = resolve(l.mc,port,g.string(g.arg(2)));
            const int32_t maxent = static_cast<int32_t>(g.arg(4));
            if(std::getenv("BOZO_TRACE_MC"))std::fprintf(stderr,"[mc] dir port=%d slot=%u path=%s mode=%u max=%d\n",port,g.arg(1),name.c_str(),g.arg(3),maxent);
            const uint32_t table = g.arg(5);
            if (!ready(l, port) || !validSlot(l,g.arg(1)))
            {
                finish(l, kFnGetDir, -4);
                g.ret(0);
                return;
            }
            McCard &card = l.mc.card(port);
            std::vector<std::pair<std::string, McCard::Entry>> found;
            const std::string dir = parentOf(name), pattern = baseOf(name);
            const bool wildcard = pattern.find_first_of("*?") != std::string::npos;
            if (wildcard && (dir.empty() || card.find(dir)))
            {
                McCard::Entry self;
                self.directory = true;
                self.attributes = kAttrDirectory;
                self.created = self.modified = l.mc.date;
                if (const McCard::Entry *d = dir.empty() ? nullptr : card.find(dir))
                    self.created = d->created, self.modified = d->modified;
                if (matches(pattern.c_str(), "."))
                    found.emplace_back(".", self);
                if (matches(pattern.c_str(), ".."))
                    found.emplace_back("..", self);
                for (const std::string &p : card.list(dir))
                    if (matches(pattern.c_str(), baseOf(p).c_str()))
                        found.emplace_back(baseOf(p), *card.find(p));
            }
            else if (!wildcard)
                if (const McCard::Entry *e = card.find(name))
                    found.emplace_back(baseOf(name), *e);
            if(g.arg(3)==0)l.mc.directoryCursor[port]=0;
            uint32_t index=0;
            int32_t n = 0;
            for (const auto &[entryName, e] : found)
            {
                if(index++<l.mc.directoryCursor[port])continue;
                if (n >= maxent)
                    break;
                putEntry(g, table + 64u * static_cast<uint32_t>(n), entryName, e);
                ++n;
            }
            l.mc.directoryCursor[port]+=uint32_t(n);
            const bool folderMissing = wildcard && !dir.empty() && !card.find(dir);
            finish(l, kFnGetDir, n ? n : wildcard && !folderMissing ? 0 : -4);
            g.ret(0);
        }

        // sceMcSync(mode, cmd, result): the operation finished in its call; its result handed over. Returns 1.
        void sync(Libraries &l, Guest &g)
        {
            if(std::getenv("BOZO_TRACE_MC"))std::fprintf(stderr,"[mc] sync pending=%d fn=%d result=%d cmd=%x out=%x ra=%x\n",l.mc.pending,l.mc.function,l.mc.result,g.arg(1),g.arg(2),g.context()->r[31].uw[0]);
            if (!l.mc.pending) { g.ret(-1); return; }
            if (g.arg(1)) g.write32(g.arg(1), static_cast<uint32_t>(l.mc.function));
            if (g.arg(2))
                g.write32(g.arg(2), static_cast<uint32_t>(l.mc.result));
            l.mc.pending = false;
            g.ret(1);
        }

        // ---- What the save run needs (no call log of it yet: not checked) ----

        void chdir(Libraries &l, Guest &g) {
            const int port=static_cast<int>(g.arg(0));
            const auto path=resolve(l.mc,port,g.string(g.arg(2)));
            int32_t result=-4;
            if(ready(l,port) && validSlot(l,g.arg(1))) {
                const auto *entry=l.mc.card(port).find(path);
                if(path.empty() || (entry && entry->directory)) {
                    const std::string old="/"+l.mc.currentDirectory[port];
                    if(g.arg(3))g.write(g.arg(3),old.c_str(),uint32_t(old.size()+1));
                    l.mc.currentDirectory[port]=path;result=0;
                }
            }
            finish(l,kFnChdir,result);g.ret(0);
        }

        // sceMcMkdir(port, slot, name): 0, or -4 when the parent is missing, -8 when it exists.
        void mkdir(Libraries &l, Guest &g)
        {
            const int port = static_cast<int>(g.arg(0));
            const std::string name = resolve(l.mc,port,g.string(g.arg(2)));
            int32_t r = -4;
            if (ready(l, port) && validSlot(l,g.arg(1)))
            {
                McCard &card = l.mc.card(port);
                const std::string parent = parentOf(name);
                if (card.exists(name))
                    r = -8;
                else if (parent.empty() || card.exists(parent))
                {
                    McCard::Entry &e = card.create(name, true, l.mc.date);
                    e.attributes = kAttrDirectory;
                    card.changed(name);
                    r = 0;
                }
            }
            finish(l, kFnMkdir, r);
            g.ret(0);
        }

        // sceMcOpen(port, slot, name, mode): a descriptor, or -4 (no such file and no CREAT, 0x200).
        void open(Libraries &l, Guest &g)
        {
            const int port = static_cast<int>(g.arg(0));
            const std::string name = resolve(l.mc,port,g.string(g.arg(2)));
            const int32_t mode = static_cast<int32_t>(g.arg(3));
            if (std::getenv("BOZO_TRACE_MC")) std::fprintf(stderr,"[mc] open port=%d name=%s mode=%x\n",port,name.c_str(),mode);
            int32_t r = -4;
            if (ready(l, port) && validSlot(l,g.arg(1)))
            {
                McCard &card = l.mc.card(port);
                McCard::Entry *e = card.find(name);
                if (!e && (mode & 0x200) && (parentOf(name).empty() || card.exists(parentOf(name))))
                {
                    e = &card.create(name, false, l.mc.date);
                    e->attributes = kAttrFile;
                    card.changed(name);
                }
                if (e && !e->directory)
                {
                    if (mode & 0x400) // TRUNC
                        e->data.clear();
                    const int32_t fd = l.mc.nextDescriptor++;
                    l.mc.files[fd] = McState::Open{port, name, 0, mode};
                    r = fd;
                }
            }
            finish(l, kFnOpen, r);
            g.ret(0);
        }

        void close(Libraries &l, Guest &g)
        {
            const int32_t fd = static_cast<int32_t>(g.arg(0));
            auto it = l.mc.files.find(fd);
            int32_t r = -4;
            if (it != l.mc.files.end())
            {
                l.mc.card(it->second.port).changed(it->second.path);
                l.mc.files.erase(it);
                r = 0;
            }
            finish(l, kFnClose, r);
            g.ret(0);
        }

        // sceMcRead(fd, buffer, size), sceMcWrite(fd, buffer, size): the bytes moved.
        void read(Libraries &l, Guest &g)
        {
            auto it = l.mc.files.find(static_cast<int32_t>(g.arg(0)));
            int32_t r = -4;
            if (it != l.mc.files.end())
                if (McCard::Entry *e = l.mc.card(it->second.port).find(it->second.path))
                {
                    const uint32_t at = it->second.position, size = g.arg(2);
                    const uint32_t n = at < e->data.size() ? std::min<uint32_t>(size, static_cast<uint32_t>(e->data.size()) - at) : 0;
                    if (n)
                        g.write(g.arg(1), e->data.data() + at, n);
                    it->second.position += n;
                    r = static_cast<int32_t>(n);
                }
            finish(l, kFnRead, r);
            g.ret(0);
        }

        void write(Libraries &l, Guest &g)
        {
            auto it = l.mc.files.find(static_cast<int32_t>(g.arg(0)));
            int32_t r = -4;
            if (it != l.mc.files.end())
                if (McCard::Entry *e = l.mc.card(it->second.port).find(it->second.path))
                {
                    const uint32_t at = it->second.position, size = g.arg(2);
                    if (e->data.size() < at + size)
                        e->data.resize(at + size);
                    if (size)
                        std::memcpy(e->data.data() + at, g.in(g.arg(1), size), size);
                    e->modified = l.mc.date;
                    it->second.position += size;
                    l.mc.card(it->second.port).changed(it->second.path);
                    r = static_cast<int32_t>(size);
                }
            finish(l, kFnWrite, r);
            g.ret(0);
        }

        // sceMcSeek(fd, offset, origin): the new position; origin 0 the start, 1 the position, 2 the end.
        void seek(Libraries &l, Guest &g)
        {
            auto it = l.mc.files.find(g.sarg(0));
            int32_t r = -4;
            if (it != l.mc.files.end())
                if (const McCard::Entry *e = l.mc.card(it->second.port).find(it->second.path))
                {
                    const int64_t base = g.arg(2) == 0 ? 0 : g.arg(2) == 1 ? int64_t(it->second.position) : int64_t(e->data.size());
                    const int64_t at = base + g.sarg(1);
                    if (at >= 0 && at <= INT32_MAX)
                    {
                        it->second.position = static_cast<uint32_t>(at);
                        r = static_cast<int32_t>(at);
                    }
                }
            finish(l, kFnSeek, r);
            g.ret(0);
        }

        void flush(Libraries &l, Guest &g)
        {
            finish(l, kFnFlush, l.mc.files.count(g.sarg(0)) ? 0 : -4);
            g.ret(0);
        }

        bool isOpen(const McState &mc, int port, const std::string &path)
        {
            for (const auto &[fd, f] : mc.files)
                if (f.port == port && f.path == path)
                    return true;
            return false;
        }

        // sceMcDelete(port, slot, name): 0, -4 no such entry, -5 open, -6 a directory with entries.
        void remove(Libraries &l, Guest &g)
        {
            const int port = g.sarg(0);
            const std::string name = resolve(l.mc, port, g.string(g.arg(2)));
            int32_t r = -4;
            if (ready(l, port) && validSlot(l, g.arg(1)))
            {
                McCard &card = l.mc.card(port);
                if (const McCard::Entry *e = card.find(name))
                {
                    if (isOpen(l.mc, port, name))
                        r = -5;
                    else if (e->directory && !card.list(name).empty())
                        r = -6;
                    else
                    {
                        card.remove(name);
                        r = 0;
                    }
                }
            }
            finish(l, kFnDelete, r);
            g.ret(0);
        }

        // sceMcRename(port, slot, name, newName): the new name in the same directory. 0, -4 no such entry, -5 open,
        // -8 the new name is taken.
        void rename(Libraries &l, Guest &g)
        {
            const int port = g.sarg(0);
            const std::string from = resolve(l.mc, port, g.string(g.arg(2)));
            const std::string base = g.string(g.arg(3));
            const std::string dir = parentOf(from), to = dir.empty() ? base : dir + "/" + base;
            int32_t r = -4;
            if (ready(l, port) && validSlot(l, g.arg(1)) && l.mc.card(port).exists(from))
            {
                if (isOpen(l.mc, port, from))
                    r = -5;
                else if (l.mc.card(port).exists(to))
                    r = -8;
                else
                {
                    l.mc.card(port).rename(from, to);
                    r = 0;
                }
            }
            finish(l, kFnRename, r);
            g.ret(0);
        }

        McDate getDate(const uint8_t *d)
        {
            McDate t;
            t.second = d[1];
            t.minute = d[2];
            t.hour = d[3];
            t.day = d[4];
            t.month = d[5];
            t.year = static_cast<uint16_t>(d[6] | (d[7] << 8));
            return t;
        }

        // sceMcSetFileInfo(port, slot, name, info, valid): from an sceMcTblGetDir entry, by the valid bits (1 created,
        // 2 modified, 4 attributes).
        void setFileInfo(Libraries &l, Guest &g)
        {
            const int port = g.sarg(0);
            const std::string name = resolve(l.mc, port, g.string(g.arg(2)));
            int32_t r = -4;
            if (ready(l, port) && validSlot(l, g.arg(1)))
                if (McCard::Entry *e = l.mc.card(port).find(name))
                {
                    const uint8_t *info = g.in(g.arg(3), 64);
                    const uint32_t valid = g.arg(4);
                    if (valid & 1u)
                        e->created = getDate(info);
                    if (valid & 2u)
                        e->modified = getDate(info + 8);
                    if (valid & 4u)
                        std::memcpy(&e->attributes, info + 20, 2);
                    r = 0;
                }
            finish(l, kFnSetFileInfo, r);
            g.ret(0);
        }

        void formatCard(Libraries &l, Guest &g, int32_t function, bool unformatted)
        {
            const int port = g.sarg(0);
            int32_t r = -4;
            if (ready(l, port) && validSlot(l, g.arg(1)))
            {
                for (auto f = l.mc.files.begin(); f != l.mc.files.end();)
                    f = f->second.port == port ? l.mc.files.erase(f) : std::next(f);
                l.mc.card(port).erase();
                l.mc.currentDirectory[port].clear();
                l.mc.unformatted[static_cast<size_t>(port)] = unformatted;
                r = 0;
            }
            finish(l, function, r);
            g.ret(0);
        }

        void formatC(Libraries &l, Guest &g) { formatCard(l, g, kFnFormat, false); }
        void unformat(Libraries &l, Guest &g) { formatCard(l, g, kFnUnformat, true); }

        // sceMcGetEntSpace(port, slot, path): the entries a directory still has room for; -4 no such directory.
        void getEntSpace(Libraries &l, Guest &g)
        {
            const int port = g.sarg(0);
            const std::string name = resolve(l.mc, port, g.string(g.arg(2)));
            int32_t r = -4;
            if (ready(l, port) && validSlot(l, g.arg(1)))
            {
                McCard &card = l.mc.card(port);
                const McCard::Entry *e = card.find(name);
                if (name.empty() || (e && e->directory))
                    r = static_cast<int32_t>(card.freeClusters());
            }
            finish(l, kFnEntSpace, r);
            g.ret(0);
        }

        void changeThreadPriority(Libraries &l, Guest &g)
        {
            finish(l, kFnChangePriority, 0);
            g.ret(0);
        }

        void getSlotMax(Libraries &, Guest &g) { g.ret(1); }

        void end(Libraries &l, Guest &g)
        {
            l.mc.known = {};
            g.ret(0);
        }
    }

    // ---- McCard --------------------------------------------------------------------------------------------------

    void McCard::format(const std::string &hostDirectory, const McDate &date)
    {
        entries_.clear();
        host_ = hostDirectory;
        if (host_.empty())
            return;
        std::error_code ec;
        fs::create_directories(host_, ec);
        // Load what the host folder holds (a card kept across runs).
        for (auto it = fs::recursive_directory_iterator(host_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const std::string path = fs::relative(it->path(), host_, ec).generic_string();
            Entry &e = entries_[path];
            e.created = e.modified = date;
            e.directory = it->is_directory(ec);
            e.attributes = e.directory ? kAttrDirectory : kAttrFile;
            if (!e.directory)
            {
                std::ifstream in(it->path(), std::ios::binary);
                e.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
        }
    }

    McCard::Entry *McCard::find(const std::string &path)
    {
        auto it = entries_.find(path);
        return it == entries_.end() ? nullptr : &it->second;
    }

    std::vector<std::string> McCard::list(const std::string &directory) const
    {
        std::vector<std::string> out;
        for (const auto &[path, e] : entries_)
            if (parentOf(path) == directory)
                out.push_back(path);
        return out;
    }

    McCard::Entry &McCard::create(const std::string &path, bool directory, const McDate &date)
    {
        Entry &e = entries_[path];
        e = Entry{};
        e.directory = directory;
        e.created = e.modified = date;
        return e;
    }

    void McCard::changed(const std::string &path)
    {
        if (host_.empty())
            return;
        const Entry *e = find(path);
        if (!e)
            return;
        std::error_code ec;
        const fs::path p = fs::path(host_) / path;
        if (e->directory)
            fs::create_directories(p, ec);
        else
        {
            fs::create_directories(p.parent_path(), ec);
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(e->data.data()), static_cast<std::streamsize>(e->data.size()));
        }
    }

    void McCard::remove(const std::string &path)
    {
        entries_.erase(path);
        if (host_.empty())
            return;
        std::error_code ec;
        fs::remove(fs::path(host_) / path, ec);
    }

    void McCard::rename(const std::string &from, const std::string &to)
    {
        const std::string prefix = from + "/";
        std::map<std::string, Entry> moved;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (it->first == from)
                moved[to] = std::move(it->second);
            else if (it->first.rfind(prefix, 0) == 0)
                moved[to + it->first.substr(from.size())] = std::move(it->second);
            else
            {
                ++it;
                continue;
            }
            it = entries_.erase(it);
        }
        entries_.merge(moved);
        if (host_.empty())
            return;
        std::error_code ec;
        fs::rename(fs::path(host_) / from, fs::path(host_) / to, ec);
    }

    void McCard::erase()
    {
        entries_.clear();
        if (host_.empty())
            return;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(host_, ec))
            fs::remove_all(e.path(), ec);
    }

    uint32_t McCard::freeClusters() const
    {
        uint32_t used = 0;
        for (const auto &[path, e] : entries_)
            used += e.directory ? 1u : static_cast<uint32_t>((e.data.size() + 1023) / 1024);
        return used < kCardClusters ? kCardClusters - used : 0;
    }

    McCard &McState::card(int port) { return cards[static_cast<size_t>(port & 1)]; }

    void addMc(std::vector<Function> &out)
    {
        out.push_back({"sceMcInit", kMc, init});
        out.push_back({"sceMcGetInfo", kMc, getInfo});
        out.push_back({"sceMcGetDir", kMc, getDir});
        out.push_back({"sceMcSync", kMc, sync});
        out.push_back({"sceMcMkdir", kMc, mkdir});
        out.push_back({"sceMcChdir", kMc, chdir});
        out.push_back({"sceMcOpen", kMc, open});
        out.push_back({"sceMcClose", kMc, close});
        out.push_back({"sceMcRead", kMc, read});
        out.push_back({"sceMcWrite", kMc, write});
        out.push_back({"sceMcSeek", kMc, seek});
        out.push_back({"sceMcFlush", kMc, flush});
        out.push_back({"sceMcDelete", kMc, remove});
        out.push_back({"sceMcRename", kMc, rename});
        out.push_back({"sceMcSetFileInfo", kMc, setFileInfo});
        out.push_back({"sceMcFormat", kMc, formatC});
        out.push_back({"sceMcUnformat", kMc, unformat});
        out.push_back({"sceMcGetEntSpace", kMc, getEntSpace});
        out.push_back({"sceMcChangeThreadPriority", kMc, changeThreadPriority});
        out.push_back({"sceMcGetSlotMax", kMc, getSlotMax});
        out.push_back({"sceMcEnd", kMc, end});
    }
}
