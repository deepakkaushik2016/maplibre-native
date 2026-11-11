# terrain/sky-default

Guards the sky pass (`RenderSky`) with every `sky` property at its default (`"sky": {}`): the
gradient from `horizon-color` (white) at the horizon line to `sky-color` (`#88C6FC`), placed where
maplibre-gl-js places it.

The scene is the background of maplibre-gl-js's own `sky/` render tests (`#ccaa83`, centre
35.38, 31.55, zoom 15) at pitch 70, 256x256, `maxPitch` 85. At pitch 70 the horizon line is just on
screen: the top 9 rows are sky, inside the sky-horizon gradient.

Why there is no terrain:

- **maplibre-gl-js draws the sky with or without terrain** (`painter.ts`: `if (data.sky)
  drawSky(...)`), and its own `sky/` tests have none. Native matches that.
- **With terrain, maplibre-gl-js also fogs the ground** from pitch 60 up whenever the style has a
  sky (`terrain.fragment.glsl`, `calculateFogBlendOpacity`). Fog is a separate pass (Task 6) with its
  own tests. A terrain baseline here would grade the fog too: it would fail until the fog exists,
  and the fog work would have to keep a sky test green that it changes.
- With no relief, native's camera and maplibre-gl-js's agree (native stills put the camera centre at
  sea level, gl-js clamps it to the ground), so the comparison is about the sky alone.

On macOS Metal against this baseline: byte-identical (0 pixels differ). Without the sky pass, 2,304
pixels (the 9 sky rows) differ.

## expected.png: maplibre-gl-js 6.16.0

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified.
- This style copied to `test/integration/render/tests/terrain/sky-default/style.json`.
- Rendered with the TERRAIN.md recipe:

  ```
  npm run build-dev
  UPDATE=true npx vitest run test/integration/render/render.test.ts -t "sky-"
  ```

  The same run regenerated maplibre-gl-js's 13 existing `sky/` baselines byte-identically.
