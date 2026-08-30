# Fallout 4 FFMPEG Plugin

This is a simple plugin intended to provide FFMPEG.

### Requirements
* [XMake](https://xmake.io) [3.0.0+]
* C++23 Compiler (MSVC or Clang-CL)
* Presumably whatever FFMPEG requires.

## Credit

This is based on the CommonLibF4 Plugin Template

## AI Disclosure

I used AI to assist me in writing this, all code is still written or proof-read by contributors. If an error is made, it should be assumed to be that of the contributor.

# Content Requirements.

The current plan for content expectations is as follows;

Replacement targets will mirror their actual texture names but instead of Data/Textures/.../XYZ.dds f4ffmpeg looks for a mirroring Data/Videos/.../XYZ with a compatible video extension and/or .ini file. While not yet implemented at the time of writing this, the .ini file will provide simple playlist and configuration implementations. Looping and shuffle would be supported, but not advanced methods (though that is within consideration). Simple configuration-less replacement is also possible via XYZ_# or even just XYZ, again with appropriate extension. Default setting is to loop and shuffle (as this would match what would be expected for most vanilla targets). An .ini file can however bypass any naming conventions and indeed even refer to files outside of the local directory. Note that current plans are to use relative directory structure, but it may be a consideration to swap to or permit this as a choice.

## Content Recommendations.

1. Keep resolution reasonable.
    FFMPEG is _very_ efficient, but it is still video decoding. Consider keeping your video resolution at approximately half typical texture resolution size. IE, 2K resolution packs should target 1080p video, 4K resolution packs 2160p (ironically 4K) video. Most modern GPUs can handle quite a few 1080p streams, but many 4K streams can be a bit of a gamble on lower end hardware. Moreover, if we cannot establish hardware decoding 4K will _hammer_ the CPU.

2. Do not change video resolution if you can help it.
    F4ffmpeg does most of the "magic" by targeting a specific texture resolution. We currently generate that texture resolution based on the video resolution. Typically speaking this is fine...but if you change the resolution we will be forced to change _our_ texture resolution. This will not cause some lag, it will entirely lock up the Fallout 4 renderer until we can re-establish the texture. That can be up to a full second. We _could_ eventually target a specific texture resolution and rescale the video to that, but that has it's own implications, and requires smart user configuration of their toml or worse startup profiling. At the moment this minimizes our footprint however, and I prefer it.

3. In-game video is going to be converted to RGBA8 if it isn't already.
    Fallout 4 expects RGBA8. We therefor provide RGBA8. This means if your video is not RGBA8 (it isn't) we are going to do conversion. This has extreme implications for linear targets (IE, normal maps), and will reduce end-user visual quality, likely even with our highest quality conversion settings. If you are targeting linear targets, you will need to consider our conversion processes, if and until we decide to make a specialty conversion process.

# Have any other suggestions?

If you have some suggestions or questions feel free to reach out on the G.A.R.D.E.N. discord, or to make an issue. Do keep in mind that suggestions or feature requests made are not going to necessarily be considered or guaranteed for implementation. More importantly, I am currently working on this on my own, and am not some coding master; ChatGPT does a lot of the grunt work for me at the moment, and helps me with syntax. Pull requests are also appreciated.
