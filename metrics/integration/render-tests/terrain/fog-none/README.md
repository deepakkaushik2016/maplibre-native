# terrain/fog-none

Guards that a style without `sky` renders exactly as it did before the fog, on a frame where the
horizon is on screen. (`terrain/no-sky-unchanged` covers `terrain/default`, which native renders
blank, and `terrain/default`'s own baseline is a known failure, so neither proves much.)

The scene is `terrain/sky-no-horizon`'s (the `terrain-shading` DEM at exaggeration 0, the numbered
raster, zoom 13) without its `sky` key, at pitch 78 with `maxPitch` 85: the map's far edge and the
background above it are in the frame. This is the plan's `fog-none` test and the controller's
horizon-visible no-sky test in one.

`allowed` is 0, and the actual image is byte-identical to expected.png on macOS Metal, which is
stricter than `allowed` 0 (pixelmatch ignores small differences). The first version of the Metal
fragment shader changed 123 pixels here by 1/255 (the compiler sampled the drape at a different
precision once the fog branch used the sample); the shader now samples as before, and the frame is
byte-identical again. Every other terrain render test without fog was also byte-identical to
`f70d2ae7dd1` on Metal.

## expected.png: native without the fog

Native's own render of this style on macOS Metal at `f70d2ae7dd1` (the commit before the fog).
