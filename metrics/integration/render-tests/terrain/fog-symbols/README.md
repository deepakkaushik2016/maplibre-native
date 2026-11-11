# terrain/fog-symbols

Guards how the terrain fog treats the layers drawn over the terrain: circles at four distances, at
pitch 75, with the True Earth sky.

**maplibre-gl-js 6.16.0 fogs only the terrain.** The fog is in `terrain.fragment.glsl` and nowhere
else: the symbol and circle shaders have no fog term, and `drawSymbols` / `drawCircles` pass no sky
uniforms. So the distant circles keep their full colour over the fogged ground. The plan asked for
symbols and circles to fade with the fog at their anchor; native follows gl-js and does not, and this
test pins that.

The scene is `terrain/fog-ground-blend`'s (flat terrain, banded fill; see its README for why) at
pitch 75, plus a GeoJSON circle layer (`circle-radius` 6, `#d0021b`) at latitudes 47.35, 47.37, 47.41
and 47.47. Circles rather than text: they need no glyphs, and their placement does not depend on
collision.

On macOS Metal: pixelmatch counts 0 pixels at gl-js's threshold. 6,196 pixels differ at all (at
most 38/255): the rows just below the sky's horizon line and the band edges, as in
`terrain/fog-horizon`. The circles match. Without the fog (at `f70d2ae7dd1`) the test fails with a
difference of 0.1054.

## expected.png: maplibre-gl-js 6.16.0

maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified. Rendered in the same
run as `terrain/fog-ground-blend` (see its README).
