# terrain/sky-no-horizon

Guards that the sky pass draws nothing when no part of the viewport is above the horizon line.

The scene is `terrain/default`'s (terrain-shading DEM, numbered raster, z13) at pitch 30 with
terrain exaggeration 0 and the True Earth sky. At pitch 30 the horizon line is about 2.2 viewport
heights above the centre. (`terrain/default` itself, at exaggeration 2, renders blank on native: the
camera is below the relief. Exaggeration 0 gives a picture worth comparing.)

`allowed` is 0. The draw-call side of the same guarantee is the gtest
`SkyPass.NoDrawWhenTheHorizonIsOffScreen`.

## expected.png: native without the sky

The expectation is "the same as without a sky", so the baseline is native's own render of this
style with the `sky` key removed, made on macOS Metal at `ca0a239b183` (the commit before the sky
pass). At that commit the same style with the `sky` key rendered byte-identically to it.
