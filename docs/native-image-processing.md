# Native image processing

`Material::ImageProcessing` describes PC image operations supplied by a port.
It has no dependency on console command streams, hardware register state or
console image layouts. `Renderer::draw` copies the parameters into the frame;
subsequent producer edits cannot change a published draw. Disabled operations
leave existing frame digests unchanged.

- A symmetric separable convolution has radius 1..16, a centre coefficient and
  one coefficient for each positive offset. Both sides use the same coefficient.
  `step` is a normalized source-image displacement. The port supplies normalized
  coefficients if it wants an energy-preserving kernel. Sampling uses LOD zero.
- `depthLookup` produces grayscale RGB from the current main framebuffer depth.
  Its index is `clamp(floor((1-windowDepth)*depthScale),0,255)`. Each table entry
  is normalized to 0..1. The depth image is a private OpenGL copy taken before
  this batch, not a delayed CPU depth query. This operation excludes convolution.
- `alphaSource` multiplies output alpha by another auxiliary image's green
  channel, with `clamp(green*alphaScale,0,1)`. The source must have a producer in
  the current frame and cannot be the draw target. It samples bilinearly in
  destination framebuffer coordinates. Ordinary alpha blending can then combine
  colour and opacity images without changing the main depth attachment.
- `targetWidth/targetHeight` optionally set a batch's auxiliary image dimensions.
  They travel with the draw, so a later port resize cannot change earlier work.
  Existing global target sizes remain the default. A shared-depth target must
  match main-frame dimensions. A resized image is cleared before its first draw.

Image operations require frame draws; overlay admission rejects them. Invalid
indices, dimensions, coefficients, table values and nonfinite parameters fail
before publication. Frame digests include each active operation's values.

`native_image_processing` covers immutable publication, digest changes and
mutation-free admission failures. With `BOZO_APP=ON`, the hidden video-only
`native_image_processing_gpu` fixture checks an exact three-tap kernel, current
depth lookup, independent opacity images, target resizing and optional shader
state reset. It initializes no host input. No game data is required.
