# terrain/no-sky-unchanged

Guards that a style without `sky` renders exactly as it did before the sky pass existed.

The style is `terrain/default`'s, verbatim, with `allowed` 0.

## expected.png: native before the sky pass

The baseline is native's own render of this style on macOS Metal at `ca0a239b183` (the commit before
the sky pass), not `terrain/default`'s maplibre-gl-js baseline: native does not match that one yet
(`terrain/default` is in the known-failure list), and this test is about native not changing.

Native renders this scene blank (white): at exaggeration 2 the camera, which native stills put at sea
level, is below the relief. So this test can only catch a sky pass that draws where it must not.
Native renders without a sky for every other render test in the suite too.
