# terrain/symbol-line-placement

Guards labels along lines on the terrain: drawn where the line is draped, and collided there.

The scene is a synthetic cone, 1500 m high (`tiles/terrain-cone`, see its README), at exaggeration
1, pitch 60, zoom 12, 512x512, looking north at the apex from its centre at sea level.

- **Contour labels**: `symbol-placement` `line-center`, pitched with the map (the default along a
  line), on short arcs of the 600, 900 and 1200 m contours, which are drawn as lines. The shaders
  draw their glyphs at the anchor's height; this checks that their size (the perspective ratio at
  the anchor) and their collision circles are taken there too.
- **A trail name** aligned with the viewport (`text-pitch-alignment` `viewport`) on a piece of a
  trail straight up the south-east face. Its glyphs are projected on the CPU, onto the height of the
  line under each glyph; before this change they sat on the trail's sea-level projection, some
  60 px below the red line.
- **HUT**, a point label at the centre of the 900 m contour label on the right. Both are drawn
  lifted, on top of each other, so the contour label must lose the collision. Before, its
  collision circles stayed at sea level and both were drawn.

Every labelled line is a piece that lies inside one tile at every zoom from 8 to 15 and is placed
with `line-center`, so it has one label, centred on it, whichever tiles an engine covers the view
with: native and gl-js choose different tile zooms under terrain (TERRAIN.md, tile cover and LOD),
and with `symbol-placement` `line` the label anchors would differ with them. The contours are drawn
whole only at 1200 m; of the lower ones only the half facing the camera is drawn, because the two
engines drape a thin line differently near the horizon.

| backend | difference against gl-js | at the parent commit `d3b2dd1f0a8` |
|---|---|---|
| macOS Metal | 0.000290 (76 px: the far 1200 m contour and the trail line's edges, a few pixels of "Trail") | 0.00211 |
| Linux OpenGL (llvmpipe) | 0.000282 (74 px) | |
| Linux Vulkan (lavapipe) | 0.000278 (73 px) | |

`allowed` is 0.0006: above the three backends, and a third of the failure without the lift.

## expected.png: maplibre-gl-js 6.16.0, centre at sea level

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified.
- This style copied to `test/integration/render/tests/terrain/symbol-line-placement/style.json`,
  with one addition to its metadata:
  `"operations": [["setCenterClampedToGround", false], ["setCenterElevation", 0], ["wait"]]`.
  Native still renders put the camera's centre at sea level, while gl-js clamps it to the ground by
  default (TERRAIN.md, Phase 4), so under real relief the two engines look at different scenes and
  no earlier terrain oracle could use relief. With the clamp off and the centre elevation 0, gl-js
  uses native's camera, and the draped contours line up to the pixel: the oracle grades relief.
- The DEM tiles copied to `test/integration/assets/tiles/terrain-cone/`.
- Rendered with the TERRAIN.md recipe:

  ```
  npm run build-dev
  UPDATE=true npx vitest run test/integration/render/render.test.ts -t "terrain/symbol-(line-placement|variable-anchor)"
  ```

  Rendering again made byte-identical baselines. The gl-js clone has no tracked modifications; only
  the untracked test directories and the DEM tiles were added.

The gtests `SymbolProjection.LineGlyphUsesTerrainHeight` (one glyph on a 1000 m plateau projects
where a point symbol there is drawn) and `TerrainSymbolQuery.*LineLabel*` (a tap on a line label
on the terrain finds it, and a tap on its sea-level projection does not) check the same from the
CPU side.
