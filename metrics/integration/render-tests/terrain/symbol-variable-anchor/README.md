# terrain/symbol-variable-anchor

Guards labels with a variable anchor on the terrain: drawn next to their point, on the ground, and
collided there.

The scene is `terrain/symbol-line-placement`'s cone (1500 m, exaggeration 1, pitch 60, zoom 12,
512x512; see its README) with two points, each a red circle and a label with
`text-variable-anchor` `["bottom", "top", "left", "right"]` and `text-radial-offset` 0.6:

- **Cone Peak** on the apex, which takes its first anchor (above the circle).
- **West Shoulder** at 1350 m, west of the apex. Its first anchor would overlap Cone Peak's label,
  so it takes its second (below the circle); alone, it takes the first. So the boxes collide where
  the labels are drawn.

Variable anchors are projected on the CPU (the shaders draw them through an identity label-plane
matrix), and before this change they were drawn and collided at sea level: both labels about 100 px
below their circles, which the shaders lift.

| backend | difference against gl-js | at the parent commit `d3b2dd1f0a8` |
|---|---|---|
| macOS Metal | 0.000107 (28 px, edges of the far contour line) | 0.00671 |
| Linux OpenGL (llvmpipe) | 0.000111 (29 px) | |
| Linux Vulkan (lavapipe) | 0.000107 (28 px) | |

`allowed` is 0.0003, above the three backends and a twentieth of the failure.

## expected.png: maplibre-gl-js 6.16.0, centre at sea level

Made as `terrain/symbol-line-placement`'s, in the same run, from this style copied to
`test/integration/render/tests/terrain/symbol-variable-anchor/style.json` with the same
`operations` (`setCenterClampedToGround` false, `setCenterElevation` 0), which give gl-js native's
still camera. See that README.

The gtest `TerrainSymbolQuery.VariableAnchorTextDrawnAndHitOnTerrain` checks the same from the CPU
side: a variable-anchor label on a 1000 m plateau is drawn on it, and a tap there finds it.
