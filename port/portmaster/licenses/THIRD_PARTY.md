# Third-party components in this package

| Component | Where | License |
|---|---|---|
| CryEngine 1 (Far Cry) source | farcry.aarch64 | CryEngine-SDK.txt (Crytek's source release terms) |
| FFmpeg 6.1 (minimal LGPL build: Bink decoder, demuxer, resampler) | libs.aarch64/ | FFmpeg-LGPL-2.1.txt, FFmpeg-NOTICE.txt |
| SDL2 headers (the library comes from the firmware) | farcry.aarch64, recry-launcher.aarch64 | SDL2.txt |
| stb_vorbis | farcry.aarch64 | stb-vorbis-public-domain.txt |
| etcpak (texture conversion) | farcry.aarch64 | etcpak-BSD3.txt |
| bcdec (DXT decoding) | farcry.aarch64 | bcdec-MIT-or-Unlicense.txt |
| Lua 4 (part of the engine source) | farcry.aarch64 | Lua-header.txt |
| zlib | farcry.aarch64 | zlib-header.txt |
| FreeType (engine font code) | farcry.aarch64 | FreeType-notices.txt |
| Noto Sans / Noto Sans Display | fonts/ | NotoSans-OFL-1.1.txt |

Used from PortMaster at run time, not shipped here: gptokeyb / gptokeyb2, innoextract.
Far Cry's game data is not included; it comes from the user's own GOG installer.
