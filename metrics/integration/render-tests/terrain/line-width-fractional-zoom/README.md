# terrain/line-width-fractional-zoom

Guards the quarter-zoom drape key (`TerrainLoadBudget::drapeZoomStep` 0.25 in Quality and
Balanced): a drape target re-bakes when the camera zoom, floored to a quarter, changes, so paint
interpolated over zoom stays close to what the map shows without terrain.

The line's `line-width` is interpolated 2 -> 12 px over z12 -> z13. The test renders at z12, then
zooms to z12.5 (`setZoom`) and renders again. At z12.5 the line must be 7 px wide. With a
whole-zoom key the z12.5 frame keeps the z12 bake, magnified 2^0.5: a 2.8 px line.

Why this scene:

- **Two renders, not one.** A still render that starts at z12.5 bakes the drape at z12.5 and
  shows 7 px with either key. Only a zoom change within one tile zoom shows whether the drape
  re-bakes.
- **The same covering tiles at both zooms.** Any new drape target or covering tile re-bakes on
  its own, so the scene keeps them fixed. The GeoJSON source's 512 px tiles are z12 at both
  zooms. The terrain mesh would move from z13 to z14 tiles at z12.5 (raster-dem tiles cover at
  the rounded zoom); the DEM's `maxzoom` 10 and `"terrainLoadMode": "balanced"` hold it at z13
  (Balanced meshes at most 3 levels past the DEM). In Quality the mesh follows the view, so it
  moves to z14 and every target re-bakes with either key. The terrain-shading fixture has z10
  tiles under this view.
- **Exaggeration 0.** Native still renders put the camera centre at sea level and maplibre-gl-js
  clamps it to the ground, so over relief the two cameras see different scenes. On a flat mesh
  they agree. The line is still drawn into the drape targets and sampled through the terrain
  mesh.
- **A large width change.** The image comparison ignores pixels it judges anti-aliased, so a
  line about 1 px wider per side passes. 2.8 px against 7 px does not.

On macOS Metal against this baseline:

| native | diff | result |
|---|---|---|
| Balanced, quarter-zoom key | under `allowed` (3,614 line pixels; the baseline has 3,622) | pass |
| Balanced, whole-zoom key (before the quarter key) | 0.0145 | fail |
| Performance (whole-zoom key by design) | 0.0192 | fail |

## expected.png: maplibre-gl-js without terrain at z12.5

maplibre-gl-js keys its render-to-texture tiles on the tile zoom too. With terrain, the same
render-then-`setZoom` sequence shows the snapped width (a 2.8 px line), so it is not the target.
The baseline is maplibre-gl-js **without terrain**, rendered once at z12.5: the 7 px line the map
shows when nothing is draped. That is what the quarter-zoom key is for.

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified.
- This style with the `terrain` key and the `terrain` source removed, `zoom` set to 12.5 and the
  `operations` removed, copied to
  `test/integration/render/tests/terrain/line-width-fractional-zoom-no-terrain/style.json`.
- Rendered with the TERRAIN.md recipe:

  ```
  npm run build-dev
  UPDATE=true npx vitest run test/integration/render/render.test.ts -t "line-width-fractional-zoom"
  ```

The same run rendered this style as it is (with terrain and the operations) as
`line-width-fractional-zoom`: 1,469 line pixels against 3,622 without terrain, the snapped width.
