# terrain/drape-performance

Guards that `TerrainLoadMode::Performance` drapes into 512 px targets
(`TerrainLoadBudget::drapeQualityFactor` 1), matching maplibre-gl-js at its render-to-texture
`qualityFactor` 1, where Quality and Balanced drape at 1024 px.

The scene is the `terrain-shading` Innsbruck DEM and centre of the other terrain tests, with
the 4 px red line of `terrain/line-solid`, at zoom 14, pitch 45, 512x512, terrain exaggeration 0.
The style metadata sets `"terrainLoadMode": "performance"`, which the render-test runner applies.

Why this camera:

- **Exaggeration 0.** Native still renders centre the camera at sea level, but maplibre-gl-js
  clamps the centre to the ground (about 2,400 m here). Over relief, the two cameras see
  different scenes, and no `centerAltitude` lines them up (a sweep of 2,000-4,500 m left 33-58 %
  of pixels different with the numbered raster). On a flat mesh the two cameras agree, so the
  only difference left is how the drape is sampled. The line is still drawn into the drape
  targets and sampled through the terrain mesh.
- **Zoom 14, pitch 45.** The mesh magnifies the drape in the near field enough that 512 and
  1024 px targets rasterise the line differently. At zoom 12 the gap fits inside the default
  `allowed`.
- **No numbered raster.** Native and gl-js pick different raster tiles in this view (24-35 %
  of pixels differ at either resolution), which would hide the drape difference.

On macOS Metal against this baseline:

| native mode | drape px | diff | result |
|---|---|---|---|
| Performance | 512 | 0.0000801 | pass |
| Performance with the factor forced to 2 | 1024 | 0.000675 | fail |
| Balanced | 1024 | 0.00103 | fail |
| Quality | 1024 | 0.00103 | fail |

## expected.png: maplibre-gl-js at qualityFactor 1

maplibre-gl-js renders its render-to-texture tiles at `tileSize * qualityFactor`
(`src/webgl/render_to_texture.ts`: `this.rttSize = terrain.tileManager.tileSize * terrain.qualityFactor`).
`qualityFactor` is a field of `Terrain`, set to 2 in its constructor and read once, when the
map creates its `RenderToTexture`. Nothing in the API or the render-test harness sets it, so the
baseline was made from a source edit:

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (main, 2026-11-02, package
  version 6.16.0).
- In `src/render/terrain.ts`, the `Terrain` constructor: `this.qualityFactor = 2;` changed to
  `this.qualityFactor = 1;`. No other change.
- This style copied to `test/integration/render/tests/terrain/drape-performance/style.json`
  (gl-js ignores the `terrainLoadMode` key).
- Rendered with the TERRAIN.md recipe:

  ```
  npm run build-dev
  UPDATE=true npx vitest run test/integration/render/render.test.ts -t "drape-performance"
  ```

- The edit reverted and `npm run build-dev` run again.
