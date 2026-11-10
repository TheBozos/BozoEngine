# Texture replacements

This is a player's option: a folder of PNG files, each drawn instead of the texture it names. The pack is the
player's own, made on their PC from their own game. For example, a port dumps its decoded textures, and an
upscaler or hand edits turn them into replacements. Nothing of a game ships with the engine or a port.

## Files

- **Name:** each file is `<identity>.png`, with 16 hex digits. The identity is `textureIdentity`
  (`bozo/render/texture_replace.h`): the XXH64 of the texture's level-0 RGBA, seeded with the XXH64 of
  `{width, height}`. It is the same value the draw-list hash gives that texture, so a port's dump can name files
  by it.
- **Size:** the texture's own size, or 2, 4 or 8 times it each way, with the same factor both ways and no side
  over 8192. Any other size is refused once, with a message.
- **Texels:** as the port's `Texture` holds them. For a PS2 game, alpha is the GS's (`0x80` is 1.0, up to `0xff`).
  The PNG reader (`bozo/render/png_read.h`, written from the specification) reads 8-bit images of every colour
  type that are not interlaced.
- **Scope:** only a port's texture assets (`Texture::asset`) are replaced, never movie frames, render targets or
  frame reads.

## Settings

| Where | Name | Values |
|---|---|---|
| video.ini | `texture_pack` | a folder; `off`; unset: `texture_pack` in the user folder when it exists |
| environment | `PS2X_TEXTURE_PACK` | the same, over the file |
| Settings window (F2), Video tab | Texture pack | on / off (`texture_pack = off`), applies at once; shown when there is a pack |
| environment | `PS2X_TEXTURE_PACK_RELOAD=1` | lists the folder again every 2 s on its own thread; changed files are read and drawn again (development) |
| environment | `PS2X_TEXTURE_STATS=1` | logs replacements loaded, refused and kept, with texture memory, every 10 s |

## Loading and frame time

- Worker threads read and decode the files (cores / 4, one or two threads). Until a replacement is ready, the
  texture is drawn as it was: as it is, or from its upscaled copy. The present thread never waits for a file.
- The workers of both replacements and texture upscaling run at the lowest thread priority (Windows
  `THREAD_PRIORITY_LOWEST`, nice 10 elsewhere): they use what the game and the present thread leave.
- While a replacement is being read, no upscaled copy of the same texture is made (it would be dropped).
- `PS2X_TEXTURE_STATS` also counts the textures sent to the GPU since the last line (uploads and MB).
- A replacement takes precedence over texture upscaling (`texture-upscale.md`). Textures without a replacement
  still get upscaled.
- A larger copy gets the upscaled copy's mip layout: levels 1..k-1 halved from it, level k the texture's own
  level 0, then the texture's own levels.
- Larger copies sent to the GPU are limited to 4 MB per present. This covers replacements and upscaled copies
  alike, and the first one of a present always goes. Copies that finish together are therefore spread over
  several presents and never take a frame's time.
  - Without a limit, TMB's one-player route at 4x upscaling dropped to 31 presents/s for about 20 s while its
    arena's copies arrived.
  - At 12 MB, a full pack's first 150 MB still stalled presentation at the start of a match.
- A larger copy already on the GPU is shared by content. A game's texture manager gives a texture a new key when
  it moves it in its VRAM (TMB's TEX0), and a slot with the new key takes the copy that is already there instead
  of sending it again. Copies no slot uses are kept, least recently used dropped first, up to 512 MB, and are
  dropped when the replacements change.
- A material drawn without mip levels samples a larger copy down to the texture's own size (level k). A minified
  draw then looks as it did, and a magnified one gets the detail. The larger level alone would alias and cost
  bandwidth where the texture is far away.
- Loaded copies are kept on the CPU up to 512 MB, least recently used dropped first, so a texture drawn again
  later does not read its file again.

## Tests

`tests/render/texture_replace_tests.cpp` (no GPU, CI) covers:
- the PNG reader, against zlib streams made by Python's zlib: dynamic and fixed Huffman, stored blocks, all five
  row filters, RGBA, RGB, grey with alpha, and palette with tRNS;
- refusal of cut or wrong files;
- replacements read by the workers: 4x levels and the mean of a halved texel, same-size files, a refused 3x,
  non-assets, and a rescan after a change and after a removal.
