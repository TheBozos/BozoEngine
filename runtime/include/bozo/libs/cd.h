// The CD/DVD library as the native runtime answers it (libcdvd: sceCd*, and sceFsReset): sectors read from the disc
// image, the stream (sceCdSt*) delivered from it, the drive's time modelled at the level the game observes (a read
// takes a PS2 drive's time; a stream read waits for the vblank by which its sectors have arrived). Spec:
// docs/specs/libraries.md section 8.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <map>

namespace bozo::libs
{
    constexpr uint32_t kSectorBytes = 2048;
    // EE cycles a sector takes on a PS2 drive (DVD at 4x; the reference's deterministic mode charges the same:
    // reference/tmb/README.md, the Midtown Suburbs freeze).
    constexpr uint64_t kSectorCycles = 114400;

    // The game's disc: an ISO 9660 image of 2048-byte sectors (the user's own copy of the game).
    class DiscImage
    {
    public:
        ~DiscImage();
        bool open(const std::string &path, std::string &error);
        bool isOpen() const { return f_ != nullptr; }
        const std::string &path() const { return path_; }
        uint32_t sectors() const { return sectors_; }
        // ISO 9660 directory lookup, including nested paths and optional ;version suffixes.
        bool findFile(const std::string &path, uint32_t &lbn, uint32_t &bytes);
        // Reads `count` sectors from `lbn` into `out`; sectors past the end of the image read as zeros. False when
        // the image cannot be read.
        bool read(uint32_t lbn, uint32_t count, uint8_t *out);

    private:
        std::FILE *f_ = nullptr;
        std::string path_;
        uint32_t sectors_ = 0;
    };

    struct CdState
    {
        struct File { uint32_t sector = 0, bytes = 0, position = 0; };
        std::map<int32_t, File> files;
        int32_t nextFile = 3;
        DiscImage disc;
        std::string imagePath; // opened at the first call that needs a sector
        // sceCdReadClock's time: the reference's deterministic clock (2001-06-18 12:00:00) unless the host's.
        bool hostClock = false;
        bool initialized = false;

        // The drive: the last read (sceCdRead) and its error (sceCdGetError).
        int32_t error = 0;
        uint64_t readEnd = 0; // the EE cycle the last read finished
        uint32_t readBytes = 0; // bytes actually copied by the current native read
        // Native SDK completion registration; no private SDK worker/semaphores.
        uint32_t callback = 0, callbackGp = 0, callbackStack = 0;
        bool callbackConfigured = false;

        // The stream (sceCdStInit / sceCdStStart / sceCdStRead / sceCdStStop).
        uint32_t ringSectors = 0, banks = 0, iopBuffer = 0;
        bool streaming = false;
        uint32_t streamLbn = 0;      // the stream's first sector
        uint32_t delivered = 0;      // sectors handed to the game so far
        // Free mode: sectors read into the ring so far, as of `producedAt` (the drive reads at its pace from
        // sceCdStStart and pauses while the ring is full).
        uint64_t produced = 0, producedAt = 0;
        // A blocked sceCdStRead, finished as its thread resumes.
        uint32_t pendingSectors = 0, pendingBuffer = 0, pendingError = 0;

        // Brings the drive's reading up to `cycle`.
        void advance(uint64_t cycle)
        {
            if (cycle <= producedAt)
                return;
            const uint64_t room = delivered + ringSectors > produced ? delivered + ringSectors - produced : 0;
            const uint64_t n = (cycle - producedAt) / kSectorCycles;
            if (n >= room)
            {
                produced += room;
                producedAt = cycle; // full: the drive waits for the game
            }
            else
            {
                produced += n;
                producedAt += n * kSectorCycles;
            }
        }
        // The cycle by which the ring holds `sectors` past those delivered (the drive keeps reading meanwhile).
        uint64_t readyAt(uint32_t sectors) const
        {
            const uint64_t want = delivered + sectors;
            return want <= produced ? producedAt : producedAt + (want - produced) * kSectorCycles;
        }
    };
}
