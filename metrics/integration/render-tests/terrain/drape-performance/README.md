# terrain/drape-performance

`terrain/line-solid` at `TerrainLoadMode::Performance` (style metadata
`"terrainLoadMode": "performance"`, read by the render-test runner). Performance drapes each
terrain mesh tile into a 512 px target (`TerrainLoadBudget::drapeQualityFactor` 1), where
Quality and Balanced use 1024 px (factor 2).

The style is `terrain/line-solid/style.json` with only the description and the
`terrainLoadMode` key changed.

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

Checks made on the result:

- It differs from `terrain/line-solid/expected.png` (gl-js at qualityFactor 2, same scene) in
  1828 pixels, 323 of them by more than 16 in a channel, along the draped line.
- Stock gl-js (qualityFactor 2) fails against it: the 512 px drape is what this baseline grades.

## Status

Listed in the terrain known-failure list under `metrics/ignores/`: native renders this pitched, exaggerated
scene white (the camera sits below the relief), as it does `terrain/line-solid`, so the oracle
cannot grade the drape yet. The 512 px pool is proven by the gtests
`TerrainDrapePool.PerformanceModeUses512Targets` and
`TerrainDrapePool.ModeSwitchRebuildsPoolWithoutLeak`.
