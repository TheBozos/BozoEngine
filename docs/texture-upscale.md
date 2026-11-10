# Texture upscaling

A player's option, off by default: a port's texture assets are drawn from copies made 2 or 4 times as large each way.
It is not the render scale (`render_scale`, the 3D's own resolution): the two are set apart and combine.

## Settings

| Where | Name | Values |
|---|---|---|
| video.ini | `texture_upscale` | `off` (default), `2x`, `4x` |
| video.ini | `texture_filter` | `edge` (default), `smooth` |
| environment (over the file) | `PS2X_TEXTURE_UPSCALE`, `PS2X_TEXTURE_FILTER` | the same |
| Settings window (F2), Video tab | Texture upscaling, Upscaling filter | applies at once |
| code | `Renderer::setTextureUpscale(TextureUpscale{factor, filter})` | live |

`PS2X_TEXTURE_STATS=1` logs the textures sent to the GPU and the copies made every 10 s even with upscaling off (it logs
on its own while it is on).

## What is scaled

Only a `Texture` with `asset` set: decoded from the game's own texture data. `packs::decodeRecord` sets it, so a port that
decodes its textures from a texture pack has nothing to do; a port that decodes textures another way sets it on those.
A picture made at run time (a movie frame, a frame read, a render target, a backdrop) is never scaled. A port that copies
an asset into a new `Texture` (a region of it, say) copies the flag.

The copy is made from the RGBA texels the port gave, so a palette texture is scaled after its palette was applied, and
a palette change (a new `Texture`) gets its own copy. Copies are made by worker threads; until one is made the texture
is drawn as it is. Copies are kept by content (another `Texture` object with the same texels finds the same copy) up to
256 MB on the CPU (`TextureUpscaler::setBudget`).

## Filters

Both are written for BozoEngine from the published descriptions of the methods; no code is taken from any
implementation (xBR's and xBRZ's code, and most shader packs, are under licences BozoEngine cannot take).

- `edge`: edge-directed, in the family of Hyllian's xBR (2011). For each corner of a texel, the colour differences along
  the two diagonals of its 5x5 neighbourhood decide whether an edge cuts that corner, and whether it runs at 45 degrees,
  shallow or steep. The corner then takes the neighbouring colour by the area the edge's line covers in each output
  texel, computed for any power-of-two factor. Straight edges and flat areas are kept texel for texel.
- `smooth`: Catmull-Rom bicubic, each value held within its four nearest texels (no ringing).

Both blend premultiplied by alpha, so the colour of a transparent texel never reaches a visible one (no dark or coloured
halo at a cutout's edge); the alpha of a cutout's edge gets in-between values, so an alpha-tested edge follows the
smoothed shape. A texture drawn repeating is scaled with the opposite edge as its neighbour, one drawn clamped with its
own edge: the GPU keeps one copy per wrap.

## Mip levels

With a factor of 2^k the copy's levels are: 0 the scaled image, 1..k-1 halved from it, k the texture's level 0 as it is,
then the texture's own levels (or levels made from level k at upload, as without upscaling). A level the game chooses
(`Mipmap::Lod::FromQ` or `Fixed`) is moved k levels down, so where the game samples level n it still gets its level n
and where it magnifies level 0 (LOD below 0) it gets the scaled detail. `Lod::Screen` needs nothing: the GPU measures
the copy itself. `Material::uvBounds` are normalised and unchanged; a bound on a texel edge mixes at most the outer half
of an output texel with its neighbour, as bilinear filtering of the texture as it is already does.

## Cost

Measured on Soulcalibur III (Ryzen, RTX 4080 SUPER) in its port's notes. Memory is the main cost: about 4 times the
textures' GPU memory at 2x, 16 times at 4x (plus the copies kept on the CPU).
