# third_party —— 单头文件依赖（随仓库自带）

引擎不再依赖 raylib，图像 / 字体 / 音频这三块用下面这些**单头文件库**实现。
它们都是 public domain 或 MIT-0（等价于 public domain），可以直接随仓库分发。
所有文件都是**原样拷贝**，没有本地改动；升级时整份替换即可。

| 文件 | 版本 | 许可 | 用途 |
| --- | --- | --- | --- |
| `stb_image.h` | v2.30 | public domain / MIT | PNG / JPG / GIF / BMP 解码（含 GIF 多帧，见 `stbi_load_gif_from_memory`） |
| `stb_image_write.h` | v1.16 | public domain / MIT | PNG 编码（自检截图、`ExportImage`） |
| `stb_image_resize2.h` | v2.12 | public domain / MIT | 高质量重采样（`ImageResize`，毛玻璃背景的降采样） |
| `stb_truetype.h` | v1.26 | public domain / MIT | TTF/OTF 字形光栅化（`LoadFontEx`） |
| `miniaudio.h` | v0.11.21 | public domain / MIT-0 | 音频设备、解码、混音（WAV / MP3 / FLAC / Ogg Vorbis） |

## 为什么自带而不是用系统库

* **一份源码编到哪儿都能跑**：Windows（mingw 交叉编译）和 Linux 用同一套实现，
  不需要在目标机上装 libpng / libjpeg / libvorbis / OpenAL 的开发包。
* **和原来的 raylib 行为对齐**：raylib 内部也是这几个库（stb_image / stb_truetype /
  stb_image_resize2 / miniaudio），沿用同样的实现能最大程度保持画面与音质一致。

这些头文件在 `gfx/` 里各有**恰好一个**实现单元打开 `*_IMPLEMENTATION` 宏：

| 宏 | 打开位置 |
| --- | --- |
| `STB_IMAGE_IMPLEMENTATION` | `gfx/image.cpp` |
| `STB_IMAGE_WRITE_IMPLEMENTATION` | `gfx/image.cpp` |
| `STB_IMAGE_RESIZE_IMPLEMENTATION` | `gfx/image.cpp` |
| `STB_TRUETYPE_IMPLEMENTATION` | `gfx/text.cpp` |
| `MINIAUDIO_IMPLEMENTATION` | `gfx/audio.cpp` |

**不要把带实现的头文件 include 进别的 .cpp**，否则会重复定义符号。
