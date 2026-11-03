// TMB's VU0 microprograms (libraries.md section 10): the programs the game starts with VCALLMS, as native C++ on the
// VU0 state of the calling context and VU0's data memory. What each computes was read from its microcode in the ELF
// (the hierarchy image, ELF file offset 0x135070: two MPGs, 256 pairs at 0 and 135 at 0x800) and settled bit for bit
// on the call logs' records (every register and data memory word each call changes).
//
// Arithmetic. The reference computes these programs in single precision rounded toward zero, as VU0 does, with VU0's
// adder: before a sum, the operand with the smaller exponent loses its bits below the larger one's last mantissa bit
// (no guard bits). Products, quotients and roots are the IEEE operation rounded toward zero. Chains of MULA / MADDA /
// MADD are these products and sums in the microcode's order (nothing fused). A field moved with ADDbc vf0 (0 + the
// field) turns -0 into +0. The flags (MAC, status) are not part of what the reference leaves: they stay as they were.
// This file is compiled with -frounding-math -ffp-contract=off (CMakeLists.txt) so the host does exactly that.
#include "bozo/libs/libraries.h"

#include <cfenv>
#include <cmath>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        // ---- VU0 arithmetic --------------------------------------------------------------------------------------

        struct TowardZero
        {
            int saved = std::fegetround();
            TowardZero() { std::fesetround(FE_TOWARDZERO); }
            ~TowardZero() { std::fesetround(saved); }
        };

        uint32_t bits(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        float fromBits(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        // VU0's adder: the smaller-exponent operand's bits below the larger's last mantissa bit dropped, then the sum.
        float add(float a, float b)
        {
            uint32_t ua = bits(a), ub = bits(b);
            const uint32_t ea = (ua >> 23) & 0xFFu, eb = (ub >> 23) & 0xFFu;
            if (ea != 0 && eb != 0 && ea != eb)
            {
                uint32_t &small = ea > eb ? ub : ua;
                const uint32_t shift = ea > eb ? ea - eb : eb - ea;
                small = shift >= 24 ? (small & 0x80000000u) : (small & ~((1u << shift) - 1u));
                a = fromBits(ua);
                b = fromBits(ub);
            }
            return a + b;
        }
        float sub(float a, float b) { return add(a, -b); }
        // ADDbc with vf0: 0 + x (a -0 comes out +0).
        float moved(float x) { return add(0.0f, x); }
        bool negative(float v) { return std::signbit(v) && v != 0.0f; }

        struct V
        {
            float f[4] = {0, 0, 0, 0};
            float &operator[](int i) { return f[i]; }
            float operator[](int i) const { return f[i]; }
        };

        // a * s0 + b * s1 + c * s2, field by field: the MULA, MADDA, MADD chain.
        V chain3(const V &a, float s0, const V &b, float s1, const V &c, float s2)
        {
            V out;
            for (int k = 0; k < 4; ++k)
                out[k] = add(add(a[k] * s0, b[k] * s1), c[k] * s2);
            return out;
        }

        // ---- The VU0 state a program works on ----------------------------------------------------------------------

        struct Vu
        {
            V vf[32];
            int32_t vi[16] = {};
            V acc;
            float q = 0, i = 0;
            uint32_t clip = 0;
            Guest &g;

            explicit Vu(Guest &guest) : g(guest)
            {
                const R5900Context *c = g.context();
                for (int r = 0; r < 32; ++r)
                    std::memcpy(vf[r].f, c->vu0_vf[r].f, 16);
                vf[0] = V{{0, 0, 0, 1}};
                for (int r = 1; r < 16; ++r)
                    vi[r] = static_cast<int16_t>(c->vi[r]);
                std::memcpy(acc.f, c->vu0_acc.f, 16);
                q = c->vu0_q;
                i = c->vu0_i;
                clip = c->vu0_clip_flags;
            }
            // The state handed back: every register (vi as their 16 bits), ACC, Q, I and the clip flags.
            void store()
            {
                R5900Context *c = g.context();
                for (int r = 1; r < 32; ++r)
                    std::memcpy(c->vu0_vf[r].f, vf[r].f, 16);
                for (int r = 1; r < 16; ++r)
                    c->vi[r] = static_cast<uint16_t>(vi[r]);
                std::memcpy(c->vu0_acc.f, acc.f, 16);
                c->vu0_q = q;
                c->vu0_i = i;
                c->vu0_clip_flags = clip;
            }

            // Data memory, by quadword (the address wraps at 4 KB).
            uint32_t at(int32_t qword) const { return kVu0DataBase + ((static_cast<uint32_t>(qword) & 0xFFu) << 4); }
            V lq(int32_t qword) const
            {
                V v;
                std::memcpy(v.f, g.in(at(qword), 16), 16);
                return v;
            }
            void sq(const V &v, int32_t qword) { g.write(at(qword), v.f, 16); }
            int32_t ilw(int32_t qword, int field) const // ILW: a field's low 16 bits
            {
                return static_cast<int32_t>(g.read32(at(qword) + 4u * static_cast<uint32_t>(field)) & 0xFFFFu);
            }
        };

        // The rows a, b, c of a 3x3 matrix transposed in place (the diagonal and w kept), each field moved by ADDbc.
        void transpose(V &a, V &b, V &c)
        {
            const float a1 = a[1], a2 = a[2], b2 = b[2];
            a[1] = moved(b[0]);
            b[0] = moved(a1);
            a[2] = moved(c[0]);
            c[0] = moved(a2);
            b[2] = moved(c[1]);
            c[1] = moved(b2);
        }

        // m0 * v.x + m1 * v.y + m2 * v.z field by field, ACC left as the MADDAy before the MADDz leaves it.
        V transform(Vu &u, const V &m0, const V &m1, const V &m2, const V &v)
        {
            for (int k = 0; k < 4; ++k)
                u.acc[k] = add(m0[k] * v[0], m1[k] * v[1]);
            V out;
            for (int k = 0; k < 4; ++k)
                out[k] = add(u.acc[k], m2[k] * v[2]);
            return out;
        }

        // ---- The hierarchy image's programs ----------------------------------------------------------------------

        // 0x278 (hierFovTest2Asm): an object's bounding sphere (centre vf4 + vf5, radius vf5.w) in the two view frames
        // at VU vi10 + 50 and vi10 + 53, pushed out and in by the radius. vi2 and vi5 are the sign tests the microcode
        // makes of the results (its FMAND / FSAND of the MAC flags): bit 1 any of x, y, z negative, bits 7 and 6 x and y.
        void fovTest(Vu &u)
        {
            V *f = u.vf;
            const int32_t b = u.vi[10];
            u.i = 1.0f;
            for (int k = 0; k < 3; ++k)
                f[31][k] = add(f[5][k], f[4][k]);
            f[7] = u.lq(b + 53);
            f[6][0] = moved(f[5][3]);
            f[8] = u.lq(b + 54);
            f[11][3] = f[5][3] * u.i;
            f[9] = u.lq(b + 55);
            f[1] = u.lq(b + 50);
            f[2] = u.lq(b + 51);
            f[3] = u.lq(b + 52);
            f[29] = chain3(f[7], f[31][0], f[8], f[31][1], f[9], f[31][2]);
            u.acc = chain3(f[1], f[31][0], f[2], f[31][1], f[3], f[31][2]);
            f[30] = u.acc;
            f[10] = u.lq(0);
            u.vi[6] = 0xC0;
            u.vi[7] = 0x80;
            for (int k = 0; k < 3; ++k)
            {
                f[27][k] = add(f[29][k], f[6][0]);
                f[25][k] = sub(f[29][k], f[11][3]);
                f[28][k] = add(f[30][k], f[6][0]);
                f[26][k] = sub(f[30][k], f[11][3]);
            }
            auto xy = [](const V &v) { return (negative(v[0]) ? 0x80 : 0) | (negative(v[1]) ? 0x40 : 0); };
            auto any = [](const V &v) { return negative(v[0]) || negative(v[1]) || negative(v[2]) ? 0x2 : 0; };
            u.vi[3] = xy(f[27]) & u.vi[6];
            u.vi[4] = xy(f[25]) & u.vi[6];
            f[10][0] = add(f[10][0], f[27][2]);
            u.vi[2] = any(f[28]) | u.vi[3];
            u.vi[5] = any(f[26]);
            u.vi[3] = negative(f[10][0]) ? 0x80 : 0;
            u.vi[2] |= u.vi[3];
            u.vi[5] |= u.vi[4];
        }

        // 0x3a8: vf3 = vf1 + vf4 (xyz), vf6.x = (vf3 . vf3) * vf8.y.
        void scaledDistance(Vu &u)
        {
            V *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[3][k] = add(f[1][k], f[4][k]);
            for (int k = 0; k < 3; ++k)
                f[7][k] = 1.0f;
            for (int k = 0; k < 3; ++k)
                f[5][k] = f[3][k] * f[3][k];
            u.acc[0] = add(f[3][0] * f[3][0], f[7][0] * f[5][1]);
            f[6][0] = add(u.acc[0], f[7][0] * f[5][2]);
            f[6][0] = f[6][0] * f[8][1];
        }

        // 0x410 (hierMulEoForObjAsm): vf6 = the rotation at VU vi10 + 59..61 applied to vf4 (w 1); vf7-9 = VU vi10 +
        // 56..58.
        void rotatePoint(Vu &u)
        {
            V *f = u.vf;
            const int32_t b = u.vi[10];
            f[6][3] = 1.0f;
            f[1] = u.lq(b + 59);
            f[2] = u.lq(b + 60);
            f[3] = u.lq(b + 61);
            for (int k = 0; k < 3; ++k)
                u.acc[k] = add(f[1][k] * f[4][0], f[2][k] * f[4][1]);
            for (int k = 0; k < 3; ++k)
                f[6][k] = add(u.acc[k], f[3][k] * f[4][2]);
            f[7] = u.lq(b + 56);
            f[8] = u.lq(b + 57);
            f[9] = u.lq(b + 58);
        }

        // 0x450 (hierCsUpdateAsm): the camera's rotation vf11-13 stored at VU 59-61, its transpose applied to vf31 =
        // VU 10 + vf19 into vf4 (xyz); VU 50-58 (and vf11-13, vf20-25) = the three frames at VU 11-13, 15-17, 19-21
        // each applied to the rotation's rows.
        void cameraUpdate(Vu &u)
        {
            V *f = u.vf;
            f[17] = f[12];
            f[16] = f[11];
            f[15] = f[13];
            transpose(f[11], f[12], f[13]);
            f[18] = u.lq(10);
            f[20] = u.lq(11);
            f[21] = u.lq(12);
            f[22] = u.lq(13);
            f[23] = u.lq(15);
            f[24] = u.lq(16);
            f[25] = u.lq(17);
            f[26] = u.lq(19);
            for (int k = 0; k < 3; ++k)
                f[31][k] = add(f[18][k], f[19][k]);
            const V t = chain3(f[11], f[31][0], f[12], f[31][1], f[13], f[31][2]);
            for (int k = 0; k < 3; ++k)
                f[4][k] = t[k];
            u.sq(f[16], 59);
            f[28] = u.lq(21);
            f[27] = u.lq(20);
            u.sq(f[17], 60);
            u.sq(f[15], 61);
            const V rows[3] = {f[16], f[17], f[15]};
            const V frames[3][3] = {{f[20], f[21], f[22]}, {f[23], f[24], f[25]}, {f[26], f[27], f[28]}};
            static const int into[9] = {11, 12, 13, 20, 21, 22, 23, 24, 25};
            V out[9];
            for (int m = 0; m < 3; ++m)
                for (int r = 0; r < 3; ++r)
                    out[m * 3 + r] = transform(u, frames[m][0], frames[m][1], frames[m][2], rows[r]);
            for (int n = 0; n < 9; ++n)
            {
                f[into[n]] = out[n];
                u.sq(out[n], 50 + n);
            }
        }

        // 0x5a8 (hier): the root's frames: VU 50-58 = VU 11-13, 15-17, 19-21 (through vf11-13, vf20-25), VU 59-61 the
        // identity rotation (vf28, vf27, vf26), vf4 = VU 10.
        void hierRoot(Vu &u)
        {
            V *f = u.vf;
            f[26] = V{{0, 0, 1, 0}};
            f[27] = V{{0, 1, 0, 0}};
            f[28] = V{{1, 0, 0, 0}};
            static const int into[9] = {11, 12, 13, 20, 21, 22, 23, 24, 25};
            static const int from[9] = {11, 12, 13, 15, 16, 17, 19, 20, 21};
            for (int n = 0; n < 9; ++n)
                f[into[n]] = u.lq(from[n]);
            f[4] = u.lq(10);
            for (int n = 0; n < 9; ++n)
                u.sq(f[into[n]], 50 + n);
            u.sq(f[28], 59);
            u.sq(f[27], 60);
            u.sq(f[26], 61);
        }

        // 0x670 (vu0HierRotateAsm): a child's frames, twelve quadwords on from its parent's (vi10 += 12): the parent's
        // rotation (VU vi10 + 59..61) and frames (+ 50..58) applied to the rotation vf1-3; vf4 = the rotation's
        // transpose applied to vf31 = vf4 + vf5 (xyz). The registers keep what the microcode leaves in them.
        void hierRotate(Vu &u)
        {
            V *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[31][k] = add(f[4][k], f[5][k]);
            f[6] = f[1];
            f[7] = f[2];
            f[8] = f[3];
            transpose(f[1], f[2], f[3]);
            const int32_t b = u.vi[10];
            const V rot[3] = {u.lq(b + 59), u.lq(b + 60), u.lq(b + 61)};
            V frames[3][3];
            for (int m = 0; m < 3; ++m)
                for (int k = 0; k < 3; ++k)
                    frames[m][k] = u.lq(b + 50 + 3 * m + k);
            const V t = chain3(f[1], f[31][0], f[2], f[31][1], f[3], f[31][2]);
            for (int k = 0; k < 3; ++k)
                f[4][k] = t[k];
            const V axes[3] = {f[6], f[7], f[8]};
            u.vi[10] = b + 12;
            const int32_t n = u.vi[10];
            V newRot[3], newFrames[3][3];
            for (int a = 0; a < 3; ++a)
                newRot[a] = transform(u, rot[0], rot[1], rot[2], axes[a]);
            for (int m = 0; m < 3; ++m)
                for (int a = 0; a < 3; ++a)
                    newFrames[m][a] = transform(u, frames[m][0], frames[m][1], frames[m][2], axes[a]);
            f[21] = newRot[0];
            f[22] = newRot[1];
            f[23] = newRot[2];
            f[25] = newFrames[0][0];
            f[26] = newFrames[0][1];
            f[27] = newFrames[0][2];
            f[9] = newFrames[1][0];
            f[10] = newFrames[1][1];
            f[11] = newFrames[1][2];
            f[28] = newFrames[2][0];
            f[29] = newFrames[2][1];
            f[30] = newFrames[2][2];
            f[13] = frames[0][0];
            f[14] = frames[0][1];
            f[15] = frames[0][2];
            f[17] = frames[1][0];
            f[18] = frames[1][1];
            f[19] = frames[1][2];
            f[12] = frames[2][0];
            f[16] = frames[2][1];
            f[20] = frames[2][2];
            for (int a = 0; a < 3; ++a)
                u.sq(newRot[a], n + 59 + a);
            for (int m = 0; m < 3; ++m)
                for (int a = 0; a < 3; ++a)
                    u.sq(newFrames[m][a], n + 50 + 3 * m + a);
        }

        // 0x808 (hierTraverseAsm): the lights that reach an object. Of the VU 9.x light spheres (centres VU 22 + i,
        // directions VU 30 + i), at most four whose sphere holds the object's (centre vf4 + vf5, radius vf5.w) are
        // kept, in order: VU 1 + n their direction in the object's frame (vf1-3) over 1 - w (vf0 for the ambient ones,
        // VU 9.y's bit mask; none when it faces away), VU 5 + n vf4 minus the light's position in the object's frame,
        // VU 45 + n their colour (VU 36 + i). Then the light matrix (vf10, vf18, vf12, vf13) and colours (vf20-22,
        // vf28) from them; vi14 = 1 when any was kept. The other registers keep what the microcode leaves in them.
        void lights(Vu &u)
        {
            V *f = u.vf;
            f[18] = V{};
            u.vi[14] = 0;
            f[11] = f[1];
            f[21] = u.lq(10);
            f[12] = f[2];
            f[20] = u.lq(22);
            f[13] = f[3];
            u.vi[5] = u.ilw(9, 0);
            f[17] = f[2];
            f[16] = f[1];
            f[11][2] = moved(f[3][0]);
            u.sq(f[18], 1);
            f[12][2] = moved(f[3][1]);
            f[15] = f[13];
            f[13][0] = moved(f[16][2]);
            u.vi[8] = 0;
            f[11][1] = moved(f[17][0]);
            f[22] = u.lq(30);
            f[12][0] = moved(f[16][1]);
            u.sq(f[18], 2);
            f[13][1] = moved(f[17][2]);
            u.vi[3] = 1;
            for (int k = 0; k < 4; ++k)
                f[14][k] = add(f[4][k], f[5][k]);
            u.vi[9] = 4;
            f[19] = V{{1, 1, 1, 2}};
            for (int at : {3, 4, 45, 46, 47, 48})
                u.sq(f[18], at);
            u.vi[4] = 1;
            u.vi[2] = u.ilw(9, 1);
            // One light per pass; the microcode's loop ends when none is left or four are kept.
            for (;;)
            {
                for (int k = 0; k < 4; ++k)
                    f[17][k] = add(f[21][k], f[20][k]);
                u.i = -1.0f;
                f[10] = u.lq(1);
                f[15][3] = sub(1.0f, f[22][3]);
                const bool noneLeft = u.vi[5] == 0;
                f[24][3] = f[22][3] * u.i;
                f[18] = u.lq(2);
                if (noneLeft)
                    break;
                if (u.vi[9] == 0)
                {
                    for (int k = 0; k < 3; ++k)
                        u.acc[k] = add(f[11][k] * f[17][0], f[12][k] * f[17][1]);
                    break;
                }
                for (int k = 0; k < 3; ++k)
                    f[23][k] = add(add(f[11][k] * f[17][0], f[12][k] * f[17][1]), f[13][k] * f[17][2]);
                u.vi[5] -= 1;
                u.q = 1.0f / f[15][3];
                for (int k = 0; k < 3; ++k)
                {
                    u.acc[k] = add(f[11][k] * f[22][0], f[12][k] * f[22][1]);
                    f[24][k] = add(u.acc[k], f[13][k] * f[22][2]);
                }
                u.vi[6] = 0x80;
                for (int k = 0; k < 4; ++k)
                    f[26][k] = sub(f[14][k], f[23][k]);
                for (int k = 0; k < 4; ++k)
                    f[27][k] = sub(f[4][k], f[23][k]);
                u.vi[10] = 0x40;
                u.acc[3] = f[20][3] * f[20][3];
                f[28][3] = add(u.acc[3], f[5][3] * f[5][3]);
                for (int k = 0; k < 4; ++k)
                    f[15][k] = f[26][k] * f[26][k];
                f[20] = u.lq(22 + u.vi[3]);
                for (int k = 0; k < 4; ++k)
                    f[16][k] = f[26][k] * f[24][k];
                f[22] = u.lq(30 + u.vi[3]);
                for (int k = 0; k < 4; ++k)
                    f[10][k] = f[24][k] * u.q;
                u.acc[0] = f[26][0] * f[26][0];
                u.acc[0] = add(u.acc[0], f[10][0] * f[15][1]);
                u.acc[1] = add(f[16][1], f[16][0]);
                u.vi[3] += 1;
                u.acc[0] = add(u.acc[0], f[19][0] * f[15][2]);
                u.vi[12] = u.vi[4] & u.vi[2];
                f[15][0] = sub(u.acc[0], f[19][0] * f[28][3]);
                u.acc[1] = add(u.acc[1], f[19][1] * f[16][2]);
                u.vi[4] = u.vi[4] + u.vi[4];
                f[16][1] = add(u.acc[1], f[19][1] * f[5][3]);
                u.vi[7] = negative(f[15][0]) ? 0x80 : 0;
                const bool reaches = u.vi[7] != 0;
                f[15] = u.lq(36 + u.vi[3]);
                if (!reaches)
                    continue;
                u.vi[7] = negative(f[16][1]) ? 0x40 : 0;
                if (u.vi[12] != 0)
                    u.sq(f[0], 1 + u.vi[8]); // an ambient light: no direction
                else if (u.vi[7] != 0)
                    continue; // it faces away
                else
                    u.sq(f[10], 1 + u.vi[8]);
                u.sq(f[27], 5 + u.vi[8]);
                u.sq(f[15], 45 + u.vi[8]);
                u.vi[8] += 1;
                u.vi[14] = 1;
                u.vi[9] -= 1;
            }
            // The lights kept, as the light matrix and colours.
            f[13][3] = 0.0f;
            f[20] = u.lq(45);
            f[22] = f[18];
            f[21] = f[10];
            f[12] = u.lq(3);
            f[14] = u.lq(5);
            f[15] = u.lq(6);
            f[16] = u.lq(7);
            f[10][1] = moved(f[18][0]);
            f[10][3] = 0.0f;
            f[18][3] = 0.0f;
            f[18][0] = moved(f[21][1]);
            f[10][2] = moved(f[12][0]);
            f[23] = f[12];
            f[18][2] = moved(f[12][1]);
            f[25] = f[15];
            f[26] = f[16];
            f[12][0] = moved(f[21][2]);
            f[12][1] = moved(f[22][2]);
            f[12][3] = 0.0f;
            f[24] = f[14];
            f[13][0] = moved(f[21][3]);
            f[21] = u.lq(46);
            f[13][1] = moved(f[22][3]);
            f[22] = u.lq(47);
            f[13][2] = moved(f[23][3]);
            f[31] = u.lq(48);
            f[14][1] = moved(f[25][0]);
            f[14][2] = moved(f[26][0]);
            f[23] = V{{f[21][1], f[21][2], f[21][3], f[21][0]}}; // MR32
            f[15][0] = moved(f[24][1]);
            f[15][2] = moved(f[26][1]);
            f[16][0] = moved(f[24][2]);
            f[16][1] = moved(f[25][2]);
            f[28] = V{{f[20][3], f[21][3], f[22][3], f[31][3]}};
            f[20][3] = f[31][0];
            f[21][3] = f[31][1];
            f[22][3] = f[31][2];
        }

        // 0xab0 (hdHatSphereTestAsm): a sphere (centre vf11, radius vf12.w) against the segment vf12 .. vf13. vi3 =
        // 0x80 when the first end is inside the sphere, or when the second is and the segment comes at it from
        // outside; else 0.
        void hatSphere(Vu &u)
        {
            V *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[14][k] = sub(f[12][k], f[11][k]);
            f[16][0] = 1.0f;
            f[16][1] = 1.0f;
            u.vi[2] = 0x80;
            for (int k = 0; k < 3; ++k)
                f[15][k] = sub(f[13][k], f[11][k]);
            f[12][3] = f[12][3] * f[12][3];
            for (int k = 0; k < 3; ++k)
                f[17][k] = f[14][k] * f[14][k];
            for (int k = 0; k < 3; ++k)
                f[20][k] = f[15][k] * f[15][k];
            f[10][0] = f[16][0] * f[12][3];
            u.acc[0] = add(add(f[14][0] * f[14][0], f[16][0] * f[17][1]), f[16][0] * f[17][2]);
            f[18][0] = sub(u.acc[0], f[16][0] * f[10][0]);
            f[21][0] = u.acc[0];
            u.acc[0] = add(f[15][0] * f[15][0], f[16][0] * f[20][1]);
            f[20][0] = add(u.acc[0], f[16][0] * f[20][2]);
            u.vi[3] = negative(f[18][0]) ? 0x80 : 0;
            for (int k = 0; k < 3; ++k)
                f[22][k] = sub(f[12][k], f[13][k]);
            f[23][0] = sub(f[10][0], f[21][0]);
            if (u.vi[3] != 0)
                return;
            u.q = 1.0f / std::sqrt(std::fabs(f[20][0])); // RSQRT
            for (int k = 0; k < 3; ++k)
                f[17][k] = f[22][k] * f[22][k];
            u.acc[0] = add(add(f[22][0] * f[22][0], f[16][0] * f[17][1]), f[16][0] * f[17][2]);
            f[17][0] = sub(u.acc[0], f[16][0] * f[10][0]);
            for (int k = 0; k < 3; ++k)
                f[24][k] = f[15][k] * u.q;
            u.vi[3] = negative(f[17][0]) ? 0x80 : 0;
            for (int k = 0; k < 3; ++k)
                f[25][k] = f[24][k] * f[14][k];
            for (int k = 0; k < 3; ++k)
                f[26][k] = f[24][k] * f[22][k];
            u.acc[0] = add(f[24][0] * f[14][0], f[16][0] * f[25][1]);
            f[27][0] = add(u.acc[0], f[16][0] * f[25][2]);
            u.vi[7] = 0x40;
            u.acc[1] = add(f[24][1] * f[22][1], f[16][1] * f[26][0]);
            f[28][1] = add(u.acc[1], f[16][1] * f[26][2]);
            u.acc[0] = f[23][0];
            f[23][0] = add(u.acc[0], f[27][0] * f[27][0]);
            u.vi[4] = negative(f[27][0]) ? 0x80 : 0;
            u.vi[5] = negative(f[28][1]) ? 0x40 : 0;
            if (u.vi[4] != 0)
            {
                u.vi[3] = 0;
                return;
            }
            u.vi[5] = negative(f[23][0]) ? 0x80 : 0;
            if (u.vi[5] != 0)
                u.vi[3] = 0;
        }

        template <void (*P)(Vu &)>
        void run(Libraries &, Guest &g)
        {
            const TowardZero rounding;
            Vu u(g);
            P(u);
            u.store();
        }

        // The hierarchy image's programs by their start (VCALLMS's address in bytes).
        const Function kHierarchy[] = {
            {"vu0 0x278 hierFovTest2Asm", kVu0, run<fovTest>},     {"vu0 0x3a8", kVu0, run<scaledDistance>},
            {"vu0 0x410 hierMulEoForObjAsm", kVu0, run<rotatePoint>}, {"vu0 0x450 hierCsUpdateAsm", kVu0, run<cameraUpdate>},
            {"vu0 0x5a8 hier", kVu0, run<hierRoot>},               {"vu0 0x670 vu0HierRotateAsm", kVu0, run<hierRotate>},
            {"vu0 0x808 hierTraverseAsm", kVu0, run<lights>},      {"vu0 0xab0 hdHatSphereTestAsm", kVu0, run<hatSphere>},
        };
        const Function *hierarchyProgram(uint32_t pc)
        {
            switch (pc)
            {
            case 0x278: return &kHierarchy[0];
            case 0x3A8: return &kHierarchy[1];
            case 0x410: return &kHierarchy[2];
            case 0x450: return &kHierarchy[3];
            case 0x5A8: return &kHierarchy[4];
            case 0x670: return &kHierarchy[5];
            case 0x808: return &kHierarchy[6];
            case 0xAB0: return &kHierarchy[7];
            default: return nullptr;
            }
        }
    }

    // ---- Vu0State --------------------------------------------------------------------------------------------------

    void Vu0State::kick(uint32_t chcr, uint32_t tadr)
    {
        // Chain mode (CHCR bits 2-3 = 1) from one of the game's two static VIF0 chains: its microcode image.
        if (((chcr >> 2) & 3u) != 1u)
            return;
        if (tadr == kHierarchyChain)
        {
            image = Vu0Image::Hierarchy;
            hierarchyHigh = true;
        }
        else if (tadr == k2dChain)
            image = Vu0Image::TwoD;
    }

    const Function *Libraries::vu0Program(uint32_t pc) const
    {
        // The 2D image's code covers pairs 0-229 only: above it, the hierarchy image's stays (its 0x808 and 0xab0).
        Vu0Image image = vu0.image;
        if (image == Vu0Image::TwoD && pc >= Vu0State::k2dImageEnd && vu0.hierarchyHigh)
            image = Vu0Image::Hierarchy;
        return image == Vu0Image::Hierarchy ? hierarchyProgram(pc) : nullptr;
    }
}
