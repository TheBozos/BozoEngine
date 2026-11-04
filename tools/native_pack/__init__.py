# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
# Moved from the team's code in the PS2Recomp fork (tools/native_pack, tools/ps2_disc.py; phase4-inventory.md section 2.1).
"""Offline conversion of a PS2 game's 3D models and textures into the native packs BozoEngine's renderer loads
(runtime/include/bozo/render/mesh_pack.h, texture_pack.h): a port's converter parses its own game's files and emits
objects, strips and texture records through these writers. The loaders' headers describe the formats.

  mesh.py     the mesh pack writer and reader (PS2XMSH1)
  texture.py  the texture pack writer and reader (PS2XTEX1), PS2 texel formats unpacked
  ps2.py      PS2 data helpers a converter needs: VIF codes, DMA chains in a linked image, GS A+D register blocks, ELF
              segments

A port's tool imports them with BozoEngine's tools directory on sys.path:

  sys.path.insert(0, os.path.join(<BozoEngine>, 'tools'))
  from native_pack import mesh, texture, ps2
"""
