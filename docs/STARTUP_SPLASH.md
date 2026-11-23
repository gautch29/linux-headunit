<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Linux Head Unit startup artwork

The startup emblem is original vector artwork in
`assets/branding/headunit/logo.svg`. The current source embeds that geometry in
the GTK/GSK page and in the cluster greeting. It ships without pre-rendered
videos or private artwork.

## Live sequence

The native page can create a one-shot decoration on each configured output
using its own already-running GTK renderer. Both surfaces share a monotonic
timeline. Motion is cosmetic. First scene construction and draw counts
describe submission by the GTK page. Readiness and visible scanout need their
own observations.

A missing output, unavailable GPU renderer or render failure releases the
decoration back to the existing native page. Restarts during the same boot
do not replay a previously admitted splash. Configuring
`greeting_sequence_file` selects the separate
[transparent startup/goodbye sequence](GREETING_SEQUENCE.md) and takes
precedence over this opaque splash.

## Offline export

The renderer uses Python, Pillow, NumPy and ffmpeg with libx264. Its main,
cluster and wide roles generate silent MP4s and final stills.

```sh
python3 tools/render_startup_animation.py --output /tmp/headunit-artwork
```

The offline renderer uses the same SVG polygons. The component tests compare
the embedded Python geometry against the SVG and check bounded trace and
one-attempt admission. The optional isolated Wayland test checks decoration,
output ownership and retirement using synthetic native/phone surfaces.

## Connected-session cluster branding

The signed greeting path uses a separate opt-in manufacturer decoration. The
host matches one supported scene layout and inserts the original Linux Head Unit
emblem into that slot during the logo phase. The existing signed script,
Apple layer, divider and motion remain externally supplied and unmodified.
See [signed greeting](ULTRA_GREETING.md).
