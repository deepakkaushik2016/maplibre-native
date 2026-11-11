# terrain/sky-colors

Guards the sky pass with the True Earth sky of the outdoor-styles spec (section 4): `sky-color`
`#3E7CC0`, `horizon-color` `#DCE7EF`, `sky-horizon-blend` 0.85, plus the fog and atmosphere values,
which do not colour the sky (`fog-*` colour the terrain; `atmosphere-blend` applies to globe only).

The scene is `terrain/sky-default`'s at pitch 80, where the top 70 rows are sky: most of the
109-row sky-horizon gradient (0.85 x half the viewport height), against 9 rows at pitch 70. No terrain, for the reasons in `terrain/sky-default/README.md`.

On macOS Metal against this baseline: 1,024 pixels differ by 1/255 at most (rounding in the
gradient), well inside the default `allowed`. Without the sky pass, 17,920 pixels differ.

## expected.png: maplibre-gl-js 6.16.0

- maplibre-gl-js `1e0fbefe563891b417e07b50f2956ecf957572cc` (6.16.0), unmodified.
- This style copied to `test/integration/render/tests/terrain/sky-colors/style.json`.
- Rendered in the same run as `terrain/sky-default` (see its README).
