# fmvglue

Glue dll to use ffmpeg video decoding without directly linking to ffmpeg, so that the ffmpeg version can be updated without having to recompile the program that uses it. The intended use path is: Consuming program uses LoadLibraryW() to load fmvglue.dll. fmvglue.dll directly links to libavcodec.dll etc. Then you can recompile fmvglue + ffmpeg without having to touch the consuming program.

Note that this does not allow you to circumvent licenses, you still have to adhere to ffmpeg's licensing! Whatever licensing model you compile ffmpeg as will affect the license of this code as well as the consuming program when combined with ffmpeg.

## Building

Building ffmpeg on Windows with msvc is confusing and you'll probably run into weird error messages. Here's a few pointers:

https://trac.ffmpeg.org/wiki/CompilationGuide/MSVC
https://web.archive.org/web/20240225150945/https://www.roxlu.com/2019/062/compiling-ffmpeg-with-x264-on-windows-10-using-msvc

I used this command to configure ffmpeg when I got it working, intentionally disabling a lot of the features I didn't need:

```
../src/configure --toolchain=msvc --arch=x86 --prefix=./../install --enable-shared --disable-unstable --disable-programs --disable-doc --disable-network --disable-encoders --disable-muxers --disable-bsfs --disable-protocols --enable-protocol=async --enable-protocol=concat --enable-protocol=concatf --enable-protocol=data --enable-protocol=file --enable-protocol=handle --disable-indevs --disable-outdevs --disable-filters --disable-alsa --disable-appkit --disable-avfoundation --disable-bzlib --disable-coreimage --disable-iconv --disable-libxcb --disable-metal --disable-lzma --disable-sndio --disable-schannel --disable-sdl2 --disable-securetransport --disable-xlib --enable-zlib --disable-amf --disable-audiotoolbox --disable-cuda-llvm --disable-cuvid --disable-d3d11va --disable-d3d12va --disable-dxva2 --disable-ffnvcodec --disable-libdrm --disable-nvdec --disable-nvenc --disable-v4l2-m2m --disable-vaapi --disable-vdpau --disable-videotoolbox --disable-vulkan
```

The Visual Studio project assumes ffmpeg is installed at `C:\ffmpeg\install`. Adjust as appropriate if needed.
