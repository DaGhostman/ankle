# ankle icon — provenance

The app icon (`hicolor/scalable/apps/ankle.svg`, and the
`hicolor/48x48/apps/ankle.png` rendered from it) was generated with
Recraft and then hand-cleaned. This file records how, so the icon can
be regenerated or extended into a family later.

## Direction

- Concept: joined glyphs. An ankle is a joint, and a ligature joins
  characters; the mark is a `<` chevron fused into a bracket, with a
  cursor capsule at the joint. No feet, no ankles.
- Freeform mark (no background tile), dark with one accent, subtle
  depth.

## Generation

```
Tool:        Recraft, generate_image
Model:       recraftv4_1_vector (SVG output)
Size:        1:1
Variants:    3 requested, 2 returned (this is candidate "R2")
Date:        2026-10-29
```

Prompt:

> Minimal app icon logo for a fast, lightweight terminal emulator
> called "ankle". Concept: a typographic ligature — two monospace
> glyphs fused into one continuous symbol, like "->" or "=>" melting
> into a single elegant arrow, the moment where two characters join.
> Freeform mark with no background tile, centered, generous padding.
> Deep dark slate/charcoal shapes (#1c2130 to #2c3446) with one vivid
> teal accent (#2ee6c0) highlighting the joint where the glyphs
> connect. Subtle depth: soft gradient and gentle shading only, no
> heavy 3D. Clean geometric vector, crisp at 16px. No text, no letters
> spelling words, no feet, no ankles, no body parts, no computer
> monitor. Transparent background.

## Cleanup applied to the Recraft SVG

1. Stripped the C2PA `<metadata>` block.
2. Cropped to the mark: content bbox is 2138×1741 at (1110, 1309) in
   the original 4358×4358 viewBox; the new viewBox is
   `906 906 2546 2546` (mark centered, ~8% padding), rendered at
   128×128.
3. Added a rim so the dark shapes stay visible on dark panels: every
   non-accent path is repeated underneath, filled and stroked
   (`stroke-width="60"`, round joins) with a vertical gradient
   `#6b7594` → `#333a52`. The accent capsule (`rgb(5,243,226)`) gets
   no rim.
4. `48x48/apps/ankle.png` is rendered from the SVG:
   `rsvg-convert -w 48 -h 48 scalable/apps/ankle.svg`.
