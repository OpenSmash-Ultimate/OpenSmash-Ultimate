# Original launcher artwork baselines

These fifteen PNGs were captured from the original Windows Forms launcher at
1440×810 before that implementation was removed. They are launcher artwork,
not game screenshots or game-derived content. The original source was rendered
offscreen, with animation clocks and particle seed fixed; no game or installer
was started. The reference harness previously compared these states to the shared
launcher with zero differing pixels.

`ReferenceImages.cs` compares the shared launcher's current output against these
preserved images on Windows. Only live elapsed-clock text in the build view is
masked, with the same rectangles as the retired reference harness. The graphics
renderer caption and help text are also masked: upstream now enables Vulkan on
AMD, NVIDIA and Intel. Their updated wording has separate regression checks;
settings column geometry and all other original pixels remain compared. The settings tabs appended after Storage (Developer, Game) are masked; every original tab is still compared. Portable Skia
checks still render all states, but do not claim pixel equivalence to GDI+.

Keep the baselines unchanged when editing the launcher. Changes require deliberate
visual review; do not regenerate expected images from the implementation under test.
