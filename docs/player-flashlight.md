# Projected flashlight cube storage

The player flashlight submitted a valid projected light and material passes,
but surfaces remained dark. Its generated cubemap combined a compressed DDS
source face with five raw RGBA black faces. GLES requires matching storage
across cube faces and mip levels; the incomplete cube sampled black.

Compressed cube uploads now use the existing GLES DXT decoder and RGBA8 storage,
including when the driver supports native S3TC. Ordinary 2D packing/streaming
remains unchanged. The policy applies to all compressed cubemaps and increases
their texture-memory footprint; a full 256-square six-face mip chain uses about
2 MiB. The legacy engine's Linux DXT conversion stub is not used.

Run `python3 port/tests/projected_cube_regression.py` from the repository root.
It samples real GLES textures through the production upload module: DXT1/3/5,
six faces, three mip levels, native/software DXT modes and generated raw black
faces. It passes; the pre-fix upload module fails the same sample assertion
(use --upload to select an earlier GLTexUpload.cpp). Desktop and ARM builds pass.

A Bunker off/on/off capture at 640x480 visibly illuminates the wall and restores
baseline lighting on switch-off. User confirmed flashlight operation on H700
and described the cost as small in play. Performance impact has not been measured;
no FPS, memory or sustained-performance acceptance claim is made.

Installed ARM build SHA-256:
`dab2dc91c32371db4c90b73db2ad6f03e9acc7be2722575e1f479148f24ef729`.
