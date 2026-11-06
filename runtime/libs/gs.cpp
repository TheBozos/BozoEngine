// Native libgraph ABI compatibility. SetDef functions write the SDK's output structures because
// recompiled callers can use those bytes. Nothing consumes the resulting graphics packets:
// draws, textures and readbacks belong to the port's native renderer, with no GS register state.
#include "bozo/libs/libraries.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/port.h"

#include <stdexcept>

namespace bozo::libs
{
    namespace
    {
        int16_t s16(uint32_t v) { return static_cast<int16_t>(v); }
        void write64(Guest &g, uint32_t address, uint64_t v) { g.write(address, &v, 8); }
        uint64_t sx(int64_t v) { return static_cast<uint64_t>(v); }

        // GS register numbers (A+D addresses).
        enum : uint32_t
        {
            kPrim = 0x00, kRgbaq = 0x01, kXyz2 = 0x05, kXyoffset1 = 0x18, kXyoffset2 = 0x19, kPrmodecont = 0x1a,
            kScissor1 = 0x40, kScissor2 = 0x41, kDthe = 0x45, kColclamp = 0x46, kTest1 = 0x47, kTest2 = 0x48,
            kFrame1 = 0x4c, kFrame2 = 0x4d, kZbuf1 = 0x4e, kZbuf2 = 0x4f, kBitbltbuf = 0x50, kTrxpos = 0x51,
            kTrxreg = 0x52, kTrxdir = 0x53, kFinish = 0x61,
        };

        // sceGsResetGraph(mode, interlace, video mode, field mode). Mode 0 resets and sets the display mode: the
        // reference keeps {interlace, video mode, field mode, 3} as bytes at scratchpad 0x100 (the game's own libgraph
        // keeps them at sceGsGetGParam(), which the reference leaves alone) and sets the kernel's field rate.
        void resetGraph(Libraries &l, Guest &g)
        {
            const int16_t mode = s16(g.arg(0));
            if (mode == 0)
            {
                const uint8_t param[4] = {static_cast<uint8_t>(g.arg(1)), static_cast<uint8_t>(g.arg(2)),
                                          static_cast<uint8_t>(g.arg(3)), 3};
                g.write(kScratchpadBase + 0x100u, param, 4);
                if (ee::Dispatcher *d = l.dispatcher())
                {
                    if (param[1] == 3)
                        d->kernel().fieldRate(50, 1); // PAL (not in TMB's runs: TMB is NTSC only)
                    else
                        d->kernel().fieldRate(60000, 1001);
                }
            }
            g.ret(0);
        }

        // sceGsSyncVCallback(func): the previous callback; func runs at the native frame boundary (kernel.md section 2).
        void syncVCallback(Libraries &l, Guest &g)
        {
            ee::Dispatcher *d = l.dispatcher();
            g.retU(d ? d->setSyncVCallback(g.arg(0), g.context()->r[28].uw[0]) : 0);
        }

        // sceGsResetPath(): nothing to reset (no DMA, VIF or GIF); no result.
        void resetPath(Libraries &, Guest &) {}

        // sceGsSetDefDispEnv(disp, psm, w, h, dx, dy): PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR (64 bits each).
        void setDefDispEnv(Libraries &, Guest &g)
        {
            const uint32_t disp = g.arg(0);
            const int32_t psm = s16(g.arg(1)), w = s16(g.arg(2)), h = s16(g.arg(3)), dx = s16(g.arg(4)), dy = s16(g.arg(5));
            // Read only the SDK's scalar video parameters written by ResetGraph, never a packet.
            const auto *mode = g.in(kScratchpadBase + 0x100u, 3);
            const uint64_t smode2 = mode[0] == 0 ? 2 : (mode[2] ? 3 : 1);
            const uint64_t dispfb = (static_cast<uint64_t>(psm & 0xf) << 15) | (static_cast<uint64_t>(((w + 63) >> 6) & 0x3f) << 9);
            write64(g, disp, 0x66);
            write64(g, disp + 8, smode2);
            write64(g, disp + 16, dispfb);
            if (mode[1] == 2 || mode[1] == 3)
            {
                const bool ntsc = mode[1] == 2;
                const int32_t magh = w ? (w + 2559) / w : 1;
                // The horizontal start: the reference's is 652 in NTSC (the game's libgraph has 636), 656 in PAL.
                const int32_t x = (dx * magh + (ntsc ? 652 : 656)) & 0xfff;
                int32_t y, dh;
                if (mode[0] == 1)
                {
                    y = (dy + (ntsc ? 50 : 72)) & 0xfff;
                    dh = mode[2] ? h * 2 - 1 : h - 1;
                }
                else
                {
                    y = (dy + (ntsc ? 25 : 36)) & 0xfff;
                    dh = h - 1;
                }
                const uint64_t display = static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 12) |
                            (static_cast<uint64_t>(magh - 1) << 23) | (sx(magh * w - 1) << 32) | (sx(dh) << 44);
                write64(g, disp + 24, display);
            }
            write64(g, disp + 32, 0);
            g.ret(0);
        }

        // The Z buffer's base after the frame buffers (sceGszbufaddr): in 2048-word pages, two frame buffers unless
        // interlaced frame mode NTSC... as the game's libgraph decides it (its GParam's first 48 bits equal to 1).
        int32_t zbufAddress(Guest &g, int32_t psm, int32_t w, int32_t h)
        {
            const int32_t pw = ((w + 63) >= 0 ? w + 63 : w + 126) >> 6;
            const int32_t ph = (psm & 2) ? (((h + 63) >= 0 ? h + 63 : h + 126) >> 6) : (((h + 31) >= 0 ? h + 31 : h + 62) >> 5);
            const int32_t pages = pw * ph;
            const auto *mode = g.in(kScratchpadBase + 0x100u, 3);
            const bool single = mode[0] == 1 && mode[1] == 0 && mode[2] == 0;
            return single ? static_cast<int16_t>(pages) : static_cast<int16_t>(static_cast<uint32_t>(pages) << 1);
        }

        // sceGsSetDefDrawEnv / sceGsSetDefDrawEnv2(draw, psm, w, h, ztest, zpsm): eight A+D pairs for context 1 or 2.
        void drawEnv(Libraries &, Guest &g, bool second)
        {
            const uint32_t p = g.arg(0);
            const int32_t psm = s16(g.arg(1)), w = s16(g.arg(2)), h = s16(g.arg(3)), ztest = s16(g.arg(4)), zpsm = s16(g.arg(5));
            const uint64_t fbw = second ? static_cast<uint64_t>((w >> 6) & 0x3f) : static_cast<uint64_t>(((w + 63) >> 6) & 0x3f);
            write64(g, p + 0, (static_cast<uint64_t>(psm & 0xf) << 24) | (fbw << 16));
            write64(g, p + 8, second ? kFrame2 : kFrame1);
            uint64_t zbuf = sx(zbufAddress(g, psm, w, h)) | (static_cast<uint64_t>(zpsm & 0xf) << 24);
            if (ztest == 0)
                zbuf |= 1ull << 32; // ZMSK
            write64(g, p + 16, zbuf);
            write64(g, p + 24, second ? kZbuf2 : kZbuf1);
            const int64_t ofx = 2048 - static_cast<int16_t>(w >> 1), ofy = 2048 - static_cast<int16_t>(h >> 1);
            write64(g, p + 32, sx(ofx << 4) | sx(ofy << 36));
            write64(g, p + 40, second ? kXyoffset2 : kXyoffset1);
            write64(g, p + 48, sx(static_cast<int64_t>(w - 1) << 16) | sx(static_cast<int64_t>(h - 1) << 48));
            write64(g, p + 56, second ? kScissor2 : kScissor1);
            write64(g, p + 64, 1); // PRMODECONT: AC = 1, reserved bits initialized
            write64(g, p + 72, kPrmodecont);
            write64(g, p + 80, 1); // COLCLAMP: CLAMP = 1
            write64(g, p + 88, kColclamp);
            write64(g, p + 96, (psm & 2) ? 1 : 0); // dithering for 16-bit formats
            write64(g, p + 104, kDthe);
            write64(g, p + 112, ztest ? ((static_cast<uint64_t>(ztest & 3) << 17) | 0x10000) : 0x30000);
            write64(g, p + 120, second ? kTest2 : kTest1);
            g.ret(8);
        }
        void setDefDrawEnv(Libraries &l, Guest &g) { drawEnv(l, g, false); }
        void setDefDrawEnv2(Libraries &l, Guest &g) { drawEnv(l, g, true); }

        // sceGsSetDefClear(clear, ztest, x, y, w, h, r, g, b, a, z): a sprite over the area, Z test as asked. Returns 0.
        void setDefClear(Libraries &, Guest &g)
        {
            const uint32_t p = g.arg(0);
            const int32_t ztest = s16(g.arg(1)), x = s16(g.arg(2)), y = s16(g.arg(3)), w = s16(g.arg(4)), h = s16(g.arg(5));
            const uint64_t r = g.arg(6) & 0xff, gr = g.arg(7) & 0xff, b = g.arg(8) & 0xff, a = g.arg(9) & 0xff;
            const uint64_t z = static_cast<uint32_t>(g.arg(10));
            write64(g, p + 0, 0x30000);
            write64(g, p + 8, kTest1);
            write64(g, p + 16, 6); // PRIM: sprite
            write64(g, p + 24, kPrim);
            write64(g, p + 32, r | (gr << 8) | (b << 16) | (a << 24) | (0x3F800000ull << 32)); // Q = 1.0
            write64(g, p + 40, kRgbaq);
            const int32_t x0 = x << 4, y0 = y << 4, x1 = (x + w) << 4, y1 = (y + h) << 4;
            write64(g, p + 48, sx(x0) | (sx(y0) << 16) | (z << 32));
            write64(g, p + 56, kXyz2);
            write64(g, p + 64, sx(x1) | (sx(y1) << 16) | (z << 32));
            write64(g, p + 72, kXyz2);
            write64(g, p + 80, ztest ? ((static_cast<uint64_t>(ztest & 3) << 17) | 0x10000) : 0x30000);
            write64(g, p + 88, kTest1);
            g.ret(0); // the reference's result (the game's libgraph returns 6, the packet's quadwords)
        }

        // The transfer's size in quadwords for a pixel format, as the game's libgraph tables it (0 for the formats
        // it does not list).
        int32_t transferQwords(int32_t psm, int32_t w, int32_t h)
        {
            const int32_t px = w * h;
            switch (psm)
            {
            // T8H, T4HL and T4HH (0x1b, 0x24, 0x2c) as 32 bits: their texels sit in 32-bit words (match1p #16897863:
            // a 0x1b upload's image tag, inside TMB's texmResInit override, is four times an 8-bit one's).
            case 0x00: case 0x30: case 0x1b: case 0x24: case 0x2c: return px >> 2; // 32 bits
            case 0x01: case 0x31: return (px * 3) >> 4;              // 24 bits
            case 0x02: case 0x0a: case 0x32: case 0x3a: return px >> 3; // 16 bits
            case 0x13: return px >> 4;                               // 8 bits
            case 0x14: return px >> 5;                               // 4 bits
            default: return 0;
            }
        }

        // sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h): a GIF tag with BITBLTBUF, TRXPOS, TRXREG, TRXDIR,
        // then the image tag (its size from the format). Returns 0 (nothing written for a transfer over 32767 quadwords).
        void setDefLoadImage(Libraries &, Guest &g)
        {
            // A native renderer owns uploads at the asset boundary. There is no consumer
            // for this SDK packet; do not serialize it or dereference its output address.
            if (port::nativeGraphics(&g.runtime())) { g.ret(0); return; }
            const uint32_t p = g.arg(0);
            const int32_t dbp = s16(g.arg(1)), dbw = s16(g.arg(2)), dpsm = s16(g.arg(3)), x = s16(g.arg(4)), y = s16(g.arg(5)),
                          w = s16(g.arg(6)), h = s16(g.arg(7));
            const int32_t qwc = transferQwords(dpsm, w, h);
            if (qwc > 32767)
            {
                g.ret(0);
                return;
            }
            const uint64_t zero[2] = {0, 0};
            g.write(p, zero, 16);
            g.write(p + 80, zero, 16);
            write64(g, p + 0, 4ull | (1ull << 60));      // NLOOP 4, PACKED, NREG 1
            write64(g, p + 8, 0xe);                      // A+D
            // BITBLTBUF: the reference also sets SBW = 1 (the game's libgraph leaves the source fields 0).
            write64(g, p + 16, (1ull << 16) | (sx(dbp) << 32) | (sx(dbw) << 48) | (sx(dpsm) << 56));
            write64(g, p + 24, kBitbltbuf);
            write64(g, p + 32, (sx(x) << 32) | (sx(y) << 48));
            write64(g, p + 40, kTrxpos);
            write64(g, p + 48, sx(w) | (sx(h) << 32));
            write64(g, p + 56, kTrxreg);
            write64(g, p + 64, 0); // TRXDIR: host to GS memory
            write64(g, p + 72, kTrxdir);
            write64(g, p + 80, static_cast<uint64_t>(qwc & 0x7fff) | 0x8000 | (2ull << 58)); // EOP, IMAGE
            g.ret(0); // the reference's result (the game's libgraph returns 6)
        }

        // sceGsSetDefStoreImage(sp, sbp, sbw, spsm, x, y, w, h): VIF codes (NOP, NOP, FLUSHA, DIRECT 6), a GIF
        // tag with BITBLTBUF, TRXPOS, TRXREG, FINISH, TRXDIR (GS memory to host). Returns 0.
        void setDefStoreImage(Libraries &, Guest &g)
        {
            // Native readback is implemented at the port's caller. Unsupported consumers
            // still fail in ExecStoreImage, rather than relying on an unused packet.
            if (port::nativeGraphics(&g.runtime())) { g.ret(0); return; }
            const uint32_t p = g.arg(0);
            const int32_t sbp = s16(g.arg(1)), sbw = s16(g.arg(2)), spsm = s16(g.arg(3)), x = s16(g.arg(4)), y = s16(g.arg(5)),
                          w = s16(g.arg(6)), h = s16(g.arg(7));
            const uint64_t zero[2] = {0, 0};
            g.write(p + 16, zero, 16);
            // The reference's first VIF code is a NOP where the game's libgraph has MSKPATH3 (0x06008000).
            const uint32_t vif[4] = {0, 0, 0x13000000u, 0x50000006u};
            g.write(p, vif, 16);
            write64(g, p + 16, 5ull | 0x8000 | (1ull << 60)); // NLOOP 5, EOP, PACKED, NREG 1
            write64(g, p + 24, 0xe);
            // BITBLTBUF: the reference also sets DBW = 1 (the game's libgraph leaves the destination fields 0).
            write64(g, p + 32, sx(static_cast<int32_t>(static_cast<uint32_t>(sbw) << 16) | sbp) | (sx(spsm) << 24) | (1ull << 48));
            write64(g, p + 40, kBitbltbuf);
            write64(g, p + 48, sx(static_cast<int32_t>(static_cast<uint32_t>(y) << 16) | x));
            write64(g, p + 56, kTrxpos);
            write64(g, p + 64, sx(w) | (sx(h) << 32));
            write64(g, p + 72, kTrxreg);
            write64(g, p + 80, 0);
            write64(g, p + 88, kFinish);
            write64(g, p + 96, 1); // TRXDIR: GS memory to host
            write64(g, p + 104, kTrxdir);
            g.ret(0); // the reference's result (the game's libgraph returns 7)
        }

        // The port already submits native materials. Do not dereference the obsolete packet.
        void putDrawEnv(Libraries &, Guest &g)
        {
            g.ret(0);
        }

        // Native presentation already owns the display. Do not read the legacy display
        // structure or execute libgraph's writes to the GS's display circuits.
        void putDispEnv(Libraries &, Guest &g) { g.ret(0); }

        // sceGsSyncPath(mode, timeout): every path is idle (nothing is ever in flight). Returns 0.
        void syncPath(Libraries &, Guest &g)
        {
            g.ret(0);
        }

        // sceGsExecLoadImage(lp, src): an upload of the image at src as the load packet lp describes. Nothing in flight,
        // no GS memory: the renderer takes textures from the game's own assets (a port's packs). Returns 0, as every
        // one of the reference's (boot: 86, all made inside TMB's texmResInit / texmLoadResTexture overrides).
        void execLoadImage(Libraries &l, Guest &g)
        {
            (void)l;
            g.ret(0);
        }

        // A native port must implement readback at the original high-level caller, using its
        // render target and rectangle. Do not parse a console packet or fabricate a black image.
        void execStoreImage(Libraries &, Guest &)
        {
            throw std::runtime_error("sceGsExecStoreImage requires a native renderer readback override");
        }

        // sceGsSyncV(mode): the game waits for the next vblank, and the game frame ends there. The reference returns
        // 1 when the frames ended by then (this one included) are odd, 0 when even: settled on all of boot's 8 and
        // match1p's 2,290 calls. The wait and the frame end are the runtime's frame boundary.
        void syncV(Libraries &l, Guest &g)
        {
            g.ret(static_cast<int32_t>((l.framesEnded + 1u) & 1u));
        }
    }

    void addGs(std::vector<Function> &out)
    {
        out.push_back({"sceGsResetGraph", kGs, resetGraph});
        out.push_back({"sceGsResetPath", kGs, resetPath});
        out.push_back({"sceGsSetDefDispEnv", kGs, setDefDispEnv});
        out.push_back({"sceGsSetDefDrawEnv", kGs, setDefDrawEnv});
        out.push_back({"sceGsSetDefDrawEnv2", kGs, setDefDrawEnv2});
        out.push_back({"sceGsSetDefClear", kGs, setDefClear});
        out.push_back({"sceGsSetDefLoadImage", kGs, setDefLoadImage});
        out.push_back({"sceGsSetDefStoreImage", kGs, setDefStoreImage});
        out.push_back({"sceGsPutDrawEnv", kGs, putDrawEnv});
        out.push_back({"sceGsPutDispEnv", kGs, putDispEnv});
        out.push_back({"sceGsSyncPath", kGs, syncPath});
        out.push_back({"sceGsSyncVCallback", kGs, syncVCallback});
        out.push_back({"sceGsExecLoadImage", kGs, execLoadImage});
        out.push_back({"sceGsExecStoreImage", kGs, execStoreImage});
        Function v{"sceGsSyncV", kGs, syncV};
        v.frameBoundary = true;
        out.push_back(v);
    }
}
