# terrain-cone

A synthetic DEM for the terrain label tests (`render-tests/terrain/symbol-line-placement`,
`render-tests/terrain/symbol-variable-anchor`): a cone 1500 m high with its apex at lng 10, lat 46,
falling linearly to sea level 6 km out and flat at sea level beyond.

- Terrarium encoding (`R * 256 + G + B / 256 - 32768`), lossless PNG, 256 px tiles, z0 to z11, only
  the tiles that intersect lng 9.84 to 10.16, lat 45.89 to 46.11 (the styles declare these as the
  source's `bounds`).
- Each pixel holds the height at its centre. The distance to the apex is measured in Web Mercator
  scaled by cos(46°), so a contour of height h is the circle of radius 6000 * (1 - h / 1500) m in
  that metric; the tests' contour lines are built the same way and lie exactly on their heights.
- The tiles are also rows of `metrics/cache-style.db` (template
  `http://tiles/terrain-cone/{z}-{x}-{y}.terrain.png`, pixel ratio 1), which is what the native
  render tests read. maplibre-gl-js reads the same files from
  `test/integration/assets/tiles/terrain-cone/`.
