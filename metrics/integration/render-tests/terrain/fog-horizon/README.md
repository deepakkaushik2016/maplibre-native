# terrain/fog-horizon

Guards the terrain fog under the sky: the True Earth sky (outdoor-styles spec section 4) at pitch 80,
where the sky's horizon gradient, the full-strength fog and the fog's turn from `fog-color` to
`horizon-color` (past fog depth `horizon-fog-blend` 0.75) are all on screen. This is the first test
of the sky over terrain.

The scene is `terrain/fog-ground-blend`'s (flat terrain, banded fill; see its README for why) at
pitch 80.

On macOS Metal: pixelmatch counts 0 pixels at gl-js's threshold. 4,608 pixels differ at all (at
most 29/255):

- **Rows 61-69, just below the sky's horizon line.** gl-js shows the sky gradient there, native
  shows fully fogged terrain in `horizon-color` (`#DCE7EF`). Native draws the terrain further than
  gl-js's far plane, which is where gl-js's fog depth reaches 1. Past it the native shader holds the
  fog factor at 1 (TERRAIN.md, Sky and fog), so that terrain takes the colour the sky's gradient
  starts from, rather than overshooting the fog colour.
- **The band edges,** as in `terrain/fog-ground-blend`.

Without the fog (at `f70d2ae7dd1`) the test fails with a difference of 0.0774.

## expected.png: maplibre-gl-js 6.16.0

maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified. Rendered in the same
run as `terrain/fog-ground-blend` (see its README).
