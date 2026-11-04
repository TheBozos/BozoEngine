// The GS library calls TMB makes (libraries.md section 5). The SetDef functions build the packets the game later
// sends (environments, image transfers) exactly as the reference does, from the semantics of the game's own libgraph
// (its code in the ELF) where the reference agrees with it; there is no GS: what a renderer needs is kept in GsState.
#include "bozo/libs/libraries.h"

#include "bozo/ee/dispatcher.h"

#include <algorithm>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        int16_t s16(uint32_t v) { return static_cast<int16_t>(v); }
        void write64(Guest &g, uint32_t address, uint64_t v) { g.write(address, &v, 8); }
        uint64_t read64(Guest &g, uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, g.in(address, 8), 8);
            return v;
        }
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
                GsState &s = l.gs;
                s.reset = true;
                s.interlace = s16(g.arg(1));
                s.videoMode = s16(g.arg(2));
                s.fieldMode = s16(g.arg(3));
                const uint8_t param[4] = {static_cast<uint8_t>(s.interlace), static_cast<uint8_t>(s.videoMode),
                                          static_cast<uint8_t>(s.fieldMode), 3};
                g.write(kScratchpadBase + 0x100u, param, 4);
                if (ee::Dispatcher *d = l.dispatcher())
                {
                    if (s.videoMode == 3)
                        d->kernel().fieldRate(50, 1); // PAL (not in TMB's runs: TMB is NTSC only)
                    else
                        d->kernel().fieldRate(60000, 1001);
                }
            }
            g.ret(0);
        }

        // sceGsResetPath(): nothing to reset (no DMA, VIF or GIF); no result.
        void resetPath(Libraries &, Guest &) {}

        // sceGsSetDefDispEnv(disp, psm, w, h, dx, dy): PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR (64 bits each).
        void setDefDispEnv(Libraries &l, Guest &g)
        {
            const uint32_t disp = g.arg(0);
            const int32_t psm = s16(g.arg(1)), w = s16(g.arg(2)), h = s16(g.arg(3)), dx = s16(g.arg(4)), dy = s16(g.arg(5));
            const GsState &s = l.gs;
            GsState::Display d;
            d.address = disp;
            d.pmode = 0x66;
            d.smode2 = s.interlace == 0 ? 2 : (s.fieldMode ? 3 : 1);
            d.dispfb = (static_cast<uint64_t>(psm & 0xf) << 15) | (static_cast<uint64_t>(((w + 63) >> 6) & 0x3f) << 9);
            write64(g, disp, d.pmode);
            write64(g, disp + 8, d.smode2);
            write64(g, disp + 16, d.dispfb);
            if (s.videoMode == 2 || s.videoMode == 3)
            {
                const bool ntsc = s.videoMode == 2;
                const int32_t magh = w ? (w + 2559) / w : 1;
                // The horizontal start: the reference's is 652 in NTSC (the game's libgraph has 636), 656 in PAL.
                const int32_t x = (dx * magh + (ntsc ? 652 : 656)) & 0xfff;
                int32_t y, dh;
                if (s.interlace == 1)
                {
                    y = (dy + (ntsc ? 50 : 72)) & 0xfff;
                    dh = s.fieldMode ? h * 2 - 1 : h - 1;
                }
                else
                {
                    y = (dy + (ntsc ? 25 : 36)) & 0xfff;
                    dh = h - 1;
                }
                d.display = static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 12) |
                            (static_cast<uint64_t>(magh - 1) << 23) | (sx(magh * w - 1) << 32) | (sx(dh) << 44);
                write64(g, disp + 24, d.display);
            }
            d.bgcolor = 0;
            write64(g, disp + 32, 0);
            d.set = true;
            l.gs.display = d;
            g.ret(0);
        }

        // The Z buffer's base after the frame buffers (sceGszbufaddr): in 2048-word pages, two frame buffers unless
        // interlaced frame mode NTSC... as the game's libgraph decides it (its GParam's first 48 bits equal to 1).
        int32_t zbufAddress(const GsState &s, int32_t psm, int32_t w, int32_t h)
        {
            const int32_t pw = ((w + 63) >= 0 ? w + 63 : w + 126) >> 6;
            const int32_t ph = (psm & 2) ? (((h + 63) >= 0 ? h + 63 : h + 126) >> 6) : (((h + 31) >= 0 ? h + 31 : h + 62) >> 5);
            const int32_t pages = pw * ph;
            const bool single = s.interlace == 1 && s.videoMode == 0 && s.fieldMode == 0;
            return single ? static_cast<int16_t>(pages) : static_cast<int16_t>(static_cast<uint32_t>(pages) << 1);
        }

        // sceGsSetDefDrawEnv / sceGsSetDefDrawEnv2(draw, psm, w, h, ztest, zpsm): eight A+D pairs for context 1 or 2.
        void drawEnv(Libraries &l, Guest &g, bool second)
        {
            const uint32_t p = g.arg(0);
            const int32_t psm = s16(g.arg(1)), w = s16(g.arg(2)), h = s16(g.arg(3)), ztest = s16(g.arg(4)), zpsm = s16(g.arg(5));
            const uint64_t fbw = second ? static_cast<uint64_t>((w >> 6) & 0x3f) : static_cast<uint64_t>(((w + 63) >> 6) & 0x3f);
            write64(g, p + 0, (static_cast<uint64_t>(psm & 0xf) << 24) | (fbw << 16));
            write64(g, p + 8, second ? kFrame2 : kFrame1);
            uint64_t zbuf = sx(zbufAddress(l.gs, psm, w, h)) | (static_cast<uint64_t>(zpsm & 0xf) << 24);
            if (ztest == 0)
                zbuf |= 1ull << 32; // ZMSK
            write64(g, p + 16, zbuf);
            write64(g, p + 24, second ? kZbuf2 : kZbuf1);
            const int64_t ofx = 2048 - static_cast<int16_t>(w >> 1), ofy = 2048 - static_cast<int16_t>(h >> 1);
            write64(g, p + 32, sx(ofx << 4) | sx(ofy << 36));
            write64(g, p + 40, second ? kXyoffset2 : kXyoffset1);
            write64(g, p + 48, sx(static_cast<int64_t>(w - 1) << 16) | sx(static_cast<int64_t>(h - 1) << 48));
            write64(g, p + 56, second ? kScissor2 : kScissor1);
            write64(g, p + 64, read64(g, p + 64) | 1); // PRMODECONT: AC = 1, the rest kept
            write64(g, p + 72, kPrmodecont);
            write64(g, p + 80, read64(g, p + 80) | 1); // COLCLAMP: CLAMP = 1
            write64(g, p + 88, kColclamp);
            const uint64_t dthe = read64(g, p + 96);
            write64(g, p + 96, (psm & 2) ? (dthe | 1) : (dthe & ~1ull)); // dithering for 16-bit formats
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

        void remember(GsState &s, const GsState::Transfer &t)
        {
            if (s.transfers.size() >= 64)
                s.transfers.erase(s.transfers.begin());
            s.transfers.push_back(t);
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
        void setDefLoadImage(Libraries &l, Guest &g)
        {
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
            GsState::Transfer t;
            t.packet = p;
            t.base = static_cast<uint32_t>(dbp);
            t.width = static_cast<uint32_t>(dbw);
            t.psm = static_cast<uint32_t>(dpsm);
            t.x = x;
            t.y = y;
            t.w = w;
            t.h = h;
            remember(l.gs, t);
            ++l.gs.loadImages;
            g.ret(0); // the reference's result (the game's libgraph returns 6)
        }

        // sceGsSetDefStoreImage(sp, sbp, sbw, spsm, x, y, w, h): VIF codes (NOP, NOP, FLUSHA, DIRECT 6), a GIF
        // tag with BITBLTBUF, TRXPOS, TRXREG, FINISH, TRXDIR (GS memory to host). Returns 0.
        void setDefStoreImage(Libraries &l, Guest &g)
        {
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
            GsState::Transfer t;
            t.packet = p;
            t.toHost = true;
            t.base = static_cast<uint32_t>(sbp);
            t.width = static_cast<uint32_t>(sbw);
            t.psm = static_cast<uint32_t>(spsm);
            t.x = x;
            t.y = y;
            t.w = w;
            t.h = h;
            remember(l.gs, t);
            ++l.gs.storeImages;
            g.ret(0); // the reference's result (the game's libgraph returns 7)
        }

        // sceGsPutDrawEnv(packet): the GIF tag's A+D pairs are the environment; kept for the renderer. Returns 0.
        void putDrawEnv(Libraries &l, Guest &g)
        {
            const uint32_t p = g.arg(0);
            GsState &s = l.gs;
            const uint32_t nloop = std::min<uint32_t>(static_cast<uint32_t>(read64(g, p) & 0x7fff), 32);
            for (uint32_t i = 0; i < nloop; ++i)
            {
                const uint64_t data = read64(g, p + 16 + 16 * i);
                const uint32_t reg = static_cast<uint32_t>(read64(g, p + 24 + 16 * i) & 0xff);
                if (reg < s.registers.size())
                {
                    s.registers[reg] = data;
                    s.registerSet[reg] = true;
                }
            }
            ++s.drawEnvironments;
            g.ret(0);
        }

        // sceGsSyncPath(mode, timeout): every path is idle (nothing is ever in flight). Returns 0.
        void syncPath(Libraries &l, Guest &g)
        {
            ++l.gs.pathSyncs;
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

        // sceGsSyncV(mode): the game waits for the next vblank, and the game frame ends there. The reference returns
        // 1 when the frames ended by then (this one included) are odd, 0 when even: settled on all of boot's 8 and
        // match1p's 2,290 calls. The wait and the frame end are the runtime's frame boundary.
        void syncV(Libraries &l, Guest &g)
        {
            ++l.gs.vsyncs;
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
        out.push_back({"sceGsSyncPath", kGs, syncPath});
        out.push_back({"sceGsExecLoadImage", kGs, execLoadImage});
        Function v{"sceGsSyncV", kGs, syncV};
        v.frameBoundary = true;
        out.push_back(v);
    }
}
