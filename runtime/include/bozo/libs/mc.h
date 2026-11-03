// libmc as the native runtime answers it: two PS2 memory cards (ports 1 and 2, slot 0), formatted, backed by host
// files (or memory alone), every operation finished at once and its result handed over by sceMcSync. Spec:
// docs/specs/libraries.md section 7.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bozo::libs
{
    // A card's date (sceMcStDateTime): what file entries carry.
    struct McDate
    {
        uint8_t second = 0, minute = 0, hour = 12, day = 18, month = 6;
        uint16_t year = 2001; // the reference's deterministic date (sceCdReadClock: 2001-06-18 12:00:00)
    };

    class McCard
    {
    public:
        struct Entry
        {
            bool directory = false;
            uint16_t attributes = 0;
            McDate created, modified;
            std::vector<uint8_t> data;
        };

        // A formatted, empty card; `hostDirectory` non-empty: its files live there (loaded now, written as they
        // change), else in memory only.
        void format(const std::string &hostDirectory, const McDate &date);
        const std::string &hostDirectory() const { return host_; }

        // Paths are the card's ("BASCUS-97101/icon.sys"), without a leading slash.
        Entry *find(const std::string &path);
        bool exists(const std::string &path) { return find(path) != nullptr; }
        // The entries directly inside a directory ("" for the root), in name order.
        std::vector<std::string> list(const std::string &directory) const;
        Entry &create(const std::string &path, bool directory, const McDate &date);
        void changed(const std::string &path); // written back to the host file
        uint32_t freeClusters() const;

    private:
        std::map<std::string, Entry> entries_;
        std::string host_;
    };

    struct McState
    {
        std::array<McCard, 2> cards;
        bool formatted = false;
        McDate date; // the date new entries get (the deterministic one; the host's clock in the real runtime)
        std::string hostDirectory; // where cards live: <dir>/mc0, <dir>/mc1 (empty: in memory)

        // A card changed since the last sceMcGetInfo on its port (-1 at the first one after sceMcInit).
        std::array<bool, 2> known{};

        // The last operation's result, handed over by sceMcSync.
        bool pending = false;
        int32_t function = 0, result = 0;

        // Open files: descriptor -> (port, path, position, mode).
        struct Open
        {
            int port = 0;
            std::string path;
            uint32_t position = 0;
            int32_t mode = 0;
        };
        std::map<int32_t, Open> files;
        int32_t nextDescriptor = 1;
        std::string currentDirectory[2];

        McCard &card(int port);
    };
}
