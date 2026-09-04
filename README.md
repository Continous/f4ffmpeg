# Fallout 4 FFMPEG Plugin

This is a simple plugin intended to provide FFMPEG.

### Requirements
* [XMake](https://xmake.io) [3.0.0+]
* C++23 Compiler (MSVC or Clang-CL)
* Presumably whatever FFMPEG requires.

## Credit

This is based on the CommonLibF4 Plugin Template, so thanks to the Dear Fallout 4 community and everyone else who made commonlib possible.

Nomad from the G.A.R.D.E.N. server for being so supportive.

LichtFeld-Sudio https://github.com/MrNeRF/LichtFeld-Studio , who's libplacebo build process we basically just lifted lmao.
ffmpeg for making the best media programming library on the planet.

Github for providing free actions so that I can freeload and not have to run a VM.

## AI Disclosure

I used AI to assist me in writing this, all code is still written or proof-read by contributors. If an error is made, it should be assumed to be that of the contributor.

# Content Requirements.

The current plan for content expectations is as follows;

Replacement targets will mirror their actual texture names but instead of Data/Textures/.../XYZ.dds f4ffmpeg looks for a mirroring Data/Videos/.../XYZ with a compatible video extension and/or .ini file. The .ini file will provide simple playlist and configuration implementations. Looping and shuffle is supported, but not advanced methods (though that is within consideration for future updates). Simple configuration-less replacement is also possible via XYZ_# or even just XYZ, again with appropriate extension. Default setting is to loop and shuffle (as this would match what would be expected for most vanilla targets). An .ini file can however bypass any naming conventions and indeed even refer to files outside of the local directory. Note that current plans are to use relative directory structure, but it may be a consideration to swap to or permit this as a choice.

### Location playlists

Playlist INIs may include `[Location.<BGSLocation editor ID>]` blocks, with optional `.Playback`, `.Playlist`, and `.Transition` suffixes. The current player location is tested first, then its parent locations, so the narrowest configured location wins. `Mode=Add` appends that block's playlist entries to the global list; `Mode=Override` replaces the global list. `Loop`, `Shuffle`, and transition values supplied by the location block override only those global values. Each resolved location gets an independent lazy playback manager, so changing location changes playback without restarting unrelated screens. An INI with no global playlist and one or more location playlists is location-only, whether it is standalone or beside a same-stem video: outside a matching location, f4ffmpeg leaves Fallout's original texture in place.

Examples for how you could use this;

1. Add location-specific additions to playlists.
2. Only replace a texture in specific locations (particularly useful in combination with Nuka-Cola machine screen extra feature below.)
3. Make a television/display play a specific video in a specific location.

### Nuka-Cola machine screens

`[Extra]` can spawn Nuka-World's commercial-screen reference beside an allowlist of compatible machine base forms or placed references. Enable it with `EnableNukaColaMachineScreens` and list forms in `NukaColaMachineScreenTargets`. Entries may be base/reference EditorIDs or eight-digit hexadecimal FormIDs, such as `["00034661", "000302DC"]`; TOML array entries must be comma-separated. `NukaColaMachineScreenSourceForm` is created using Fallout's native reference-creation path at the machine's position and rotation, so its 3D and streaming lifecycle remain engine-managed. The screen texture is replaced through the normal `Data/Video/actors/dlc04/nukatron/nukaandcappycommercial01_d.*` mapping.

## Content Recommendations.

1. Keep resolution reasonable.
    FFMPEG is _very_ efficient, but it is still video decoding. Consider keeping your video resolution at approximately half typical texture resolution size. IE, 2K resolution packs should target 1080p video, 4K resolution packs 2160p (ironically 4K) video. Most modern GPUs can handle quite a few 1080p streams, but many 4K streams can be a bit of a gamble on lower end hardware. Moreover, if we cannot establish hardware decoding 4K will _hammer_ the CPU.

2. In-game video is going to be converted to RGBA8 if it isn't already.
    Fallout 4 expects RGBA8. We therefor provide RGBA8. This means if your video is not RGBA8 (it isn't) we are going to do conversion. This has extreme implications for linear targets (IE, normal maps), and will reduce end-user visual quality, likely even with our highest quality conversion settings. If you are targeting linear targets, you will need to consider our conversion processes.

3. Your video will likely be cropped and/or warped.
    Generally speaking, we do not do much UV-correction or remapping. There are some special usecases for TVs, but otherwise we just take the texture and substitute it with the video's. This means you must bare in mind UV mapping, and the effects it can and will have on your content, as well as any other weird effects. The workshop TVs are the easy example as you can toggle the three main effects via f4ffmpeg's toml.

# Have any other suggestions?

If you have some suggestions or questions feel free to reach out on the G.A.R.D.E.N. discord, or to make an issue. Do keep in mind that suggestions or feature requests made are not going to necessarily be considered or guaranteed for implementation. More importantly, I am currently working on this on my own, and am not some coding master; ChatGPT does a lot of the grunt work for me at the moment, and helps me with syntax. Pull requests are also appreciated.
