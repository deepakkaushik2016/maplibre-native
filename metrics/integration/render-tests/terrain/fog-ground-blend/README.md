# terrain/fog-ground-blend

Guards the terrain fog (maplibre-gl-js 6.16.0 `terrain.fragment.glsl`) with the True Earth sky of the
outdoor-styles spec (section 4): `fog-color` `#C6D4E0`, `fog-ground-blend` 0.35,
`horizon-fog-blend` 0.75, `horizon-color` `#DCE7EF`.

The scene is pitch 65, 256x256, `maxPitch` 85, zoom 13 over the Alps (`terrain/default`'s centre).
At pitch 65 gl-js draws the fog at half strength (`calculateFogBlendOpacity`: 0 below 60°, full from
70°), and the sky is not on screen yet (it appears from about 68.6°), so the frame grades the fog
alone. The fog starts at fog depth 0.35 and is drawn over the top third of the frame.

- **Flat terrain.** The `terrain-shading` DEM at exaggeration 0. Native stills put the camera centre
  at sea level while gl-js clamps it to the ground; under real relief the two cameras, and so the fog
  distances, would differ. At exaggeration 0 they agree.
- **A fill, not the numbered raster.** Native and gl-js pick different tile zooms for the draped
  raster in a pitched view (TERRAIN.md, tile cover and LOD), so the numbered tiles would differ
  everywhere. The ground here is a background and a GeoJSON fill of latitude bands that widen with
  distance, which look the same at any tile zoom and show how the fog grows with distance.

On macOS Metal: pixelmatch counts 0 pixels at gl-js's threshold. 1,708 pixels differ at all (at
most 28/255): the band edges, which native's drape resamples a little differently; inside the bands
the fogged colours are identical. Without the fog (at `f70d2ae7dd1`) the test fails with a
difference of 0.0508 (5.1% of the pixels).

## expected.png: maplibre-gl-js 6.16.0

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified.
- This style copied to `test/integration/render/tests/terrain/fog-ground-blend/style.json`.
- Rendered with the TERRAIN.md recipe, in one run with `terrain/fog-horizon` and `terrain/fog-symbols`:

  ```
  npm run build-dev
  UPDATE=true npx vitest run test/integration/render/render.test.ts -t "terrain/fog-(ground-blend|horizon|symbols)"
  ```

  A second run after a metadata-only edit made byte-identical baselines. The gl-js clone has no
  tracked modifications; only the untracked test directories were added.
