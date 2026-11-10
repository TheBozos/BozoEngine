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
| environment | `PS2X_TEXTURE_PACK_RELOAD=1` | lists the folder again every 2 s on its own thread; changed files are read and drawn again (development) |
| environment | `PS2X_TEXTURE_STATS=1` | logs replacements loaded, refused and kept, with texture memory, every 10 s |

## Loading and frame time

- Worker threads read and decode the files (cores / 4, one or two threads). Until a replacement is ready, the
  texture is drawn as it was: as it is, or from its upscaled copy. The present thread never waits for a file.
- A replacement takes precedence over texture upscaling (`texture-upscale.md`). Textures without a replacement
  still get upscaled.
- A larger copy gets the upscaled copy's mip layout: levels 1..k-1 halved from it, level k the texture's own
  level 0, then the texture's own levels.
- Larger copies sent to the GPU are limited to 12 MB per present. This covers replacements and upscaled copies
  alike, and the first one of a present always goes. Copies that finish together are therefore spread over
  several presents and never take a frame's time. Without this limit, TMB's one-player route at 4x upscaling
  dropped to 31 presents/s for about 20 s while its arena's copies arrived; with it, the route holds 60.
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
