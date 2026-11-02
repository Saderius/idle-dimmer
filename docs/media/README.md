# Acrylic demonstration

Captured locally on 2026-11-02 from the running v1.0.1 flyout, including the pending
slider-wheel change. The panel and material are the real app rendered by Windows;
the background is a temporary native window with slowly moving colour fields.
The helper temporarily positioned the flyout above that window and restored its
original position when it exited. No mouse or keyboard input was synthesized,
and no app settings or display-power controls were changed for this recording.

MEASURED 2026-11-02, encoded media metadata:

- `acrylic-demo.mp4`: eight seconds, 742 × 954 pixels, nominal 30 frames/s, no audio.
- `acrylic-demo.gif`: eight-second repeating loop, 540 × 694 pixels, nominal
  15 frames/s, using a shared palette and ordered dithering.
- `acrylic-demo.png`: poster from the MP4.

These frame rates describe the files, not a measurement of the display refresh
rate or the flyout's entrance animation. The MP4 preserves gradients better than
the GIF's limited palette. Frames near the beginning, middle and end were
inspected locally to check the visible panel and changing material; this does
not establish a pixel match with Windows Quick Settings.

The recording used FFmpeg's cropped `gdigrab` input, with the mouse cursor and
audio omitted. See [FFmpeg's capture documentation](https://ffmpeg.org/ffmpeg-devices.html#gdigrab).
The recording helper and intermediate files remain local under the ignored
`build-tools/acrylic-demo-2026-11-02/` directory; they are not part of the app.

The earlier `hero.gif` is the historical opaque-panel demonstration.
