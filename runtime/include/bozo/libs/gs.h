// The GS library's state as the native libgraph keeps it: no GS behind it, only what a native renderer takes later
// (the display mode, the environments the game sets, its image transfers). Spec: docs/specs/libraries.md section 5.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace bozo::libs
{
    struct GsState
    {
        // sceGsResetGraph: interlaced (1) or not, the video mode (2 NTSC, 3 PAL), field (1) or frame (0) mode.
        bool reset = false;
        int16_t interlace = 0, videoMode = 0, fieldMode = 0;

        // The display environment the game last built (sceGsSetDefDispEnv): PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR.
        struct Display
        {
            uint32_t address = 0; // the sceGsDispEnv in guest memory
            uint64_t pmode = 0, smode2 = 0, dispfb = 0, display = 0, bgcolor = 0;
            bool set = false;
        } display;

        // The GS registers the game's draw environments set (sceGsPutDrawEnv's A+D pairs, by register number):
        // FRAME_1/2, ZBUF_1/2, XYOFFSET_1/2, SCISSOR_1/2, PRMODECONT, COLCLAMP, DTHE, TEST_1/2...
        std::array<uint64_t, 0x64> registers{};
        std::array<bool, 0x64> registerSet{};
        uint64_t drawEnvironments = 0; // sceGsPutDrawEnv calls

        // Image transfers the game set up (sceGsSetDefLoadImage: host to GS memory; sceGsSetDefStoreImage: GS memory
        // to host): where in GS memory and how large. The data itself never reaches a GS: a native renderer takes
        // textures from the game's own assets.
        struct Transfer
        {
            uint32_t packet = 0;                   // the packet in guest memory
            bool toHost = false;                   // a store (GS memory to the host)
            uint32_t base = 0, width = 0, psm = 0; // buffer base (64-word blocks), width (64 pixels), pixel format
            int32_t x = 0, y = 0, w = 0, h = 0;
        };
        std::vector<Transfer> transfers; // the last 64
        uint64_t loadImages = 0, storeImages = 0;

        uint64_t pathSyncs = 0, vsyncs = 0;
    };
}
