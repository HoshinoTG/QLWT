#include "gfx.h"

#include <cstdio>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          // 只要解码，不需要编码器
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../third_party/miniaudio.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// 音频（miniaudio）
//
// 对应 raylib 的 raudio：
//   * Sound  = 整段解码到内存（短音效，可反复重播）
//   * Music  = 流式解码（BGM，默认循环）
//   * 两者的 frameCount == 0 都表示"加载失败"，这是游戏代码判断有效性的方式
//
// 声卡打不开（无声卡 / 无权限）时**不影响加载**：miniaudio 会用空设备（noDevice）
// 跑引擎，解码/度量照常，只是没有声音输出；IsAudioDeviceReady() 返回 false，
// 游戏就会跳过播放（与以前 raylib 无声卡时的行为一致）。
// ---------------------------------------------------------------------------

namespace
{
struct AudioState
{
    ma_engine engine{};
    bool engineReady = false;    // 引擎初始化成功（可能是空设备）
    bool deviceReady = false;    // 真的打开了声卡
    float masterVolume = 1.0f;
};

AudioState g;

// Sound / Music 的内部节点就放在 ma_sound 里，用 AudioStream::internal 挂着
ma_sound* nodeOf(const AudioStream& stream)
{
    return static_cast<ma_sound*>(stream.internal);
}

constexpr unsigned int kDummyFrameCount = 1;   // 取不到长度时用它表示"加载成功"
}   // namespace

void InitAudioDevice()
{
    if (g.engineReady) return;

    // 先试真实声卡
    if (ma_engine_init(nullptr, &g.engine) == MA_SUCCESS)
    {
        g.engineReady = true;
        g.deviceReady = true;
        ma_engine_set_volume(&g.engine, g.masterVolume);
        std::printf("[gfx] 音频设备已就绪\n");
        return;
    }

    // 没有声卡（或没权限）：退回空设备。解码、取长度照常，只是不出声。
    // 注意：绝不能在引擎跑着的时候整体拷贝 ma_engine —— 它内部有设备线程和指针。
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    if (ma_engine_init(&config, &g.engine) != MA_SUCCESS)
    {
        std::fprintf(stderr, "[gfx] 音频引擎初始化失败（将没有声音）\n");
        return;
    }
    g.engineReady = true;
    g.deviceReady = false;
    std::printf("[gfx] 没有可用的音频设备，静音运行（按空设备加载）\n");
}

void CloseAudioDevice()
{
    if (!g.engineReady) return;
    ma_engine_uninit(&g.engine);
    g.engineReady = false;
    g.deviceReady = false;
}

bool IsAudioDeviceReady() { return g.deviceReady; }

void SetMasterVolume(float volume)
{
    g.masterVolume = volume < 0.0f ? 0.0f : volume;
    if (g.engineReady) ma_engine_set_volume(&g.engine, g.masterVolume);
}

// ---------------------------------------------------------------------------
// 音效
// ---------------------------------------------------------------------------

Sound LoadSound(const char* fileName)
{
    Sound sound{};
    if (fileName == nullptr || !g.engineReady) return sound;

    ma_sound* node = new ma_sound{};
    if (ma_sound_init_from_file(&g.engine, fileName, MA_SOUND_FLAG_DECODE, nullptr, nullptr,
                                node) != MA_SUCCESS)
    {
        delete node;
        return sound;
    }

    ma_uint64 length = 0;
    ma_sound_get_length_in_pcm_frames(node, &length);
    sound.frameCount = length > 0 ? static_cast<unsigned int>(length) : kDummyFrameCount;
    sound.stream.internal = node;
    return sound;
}

void UnloadSound(Sound sound)
{
    ma_sound* node = nodeOf(sound.stream);
    if (node == nullptr) return;
    ma_sound_uninit(node);
    delete node;
}

void PlaySound(Sound sound)
{
    ma_sound* node = nodeOf(sound.stream);
    if (node == nullptr) return;
    // 从头重播：短音效连续触发时不会"吞"掉后一次
    ma_sound_seek_to_pcm_frame(node, 0);
    ma_sound_start(node);
}

void SetSoundVolume(Sound sound, float volume)
{
    ma_sound* node = nodeOf(sound.stream);
    if (node == nullptr) return;
    ma_sound_set_volume(node, volume < 0.0f ? 0.0f : volume);
}

bool IsSoundPlaying(Sound sound)
{
    ma_sound* node = nodeOf(sound.stream);
    if (node == nullptr) return false;
    return ma_sound_is_playing(node) == MA_TRUE;
}

// ---------------------------------------------------------------------------
// 音乐（流式）
// ---------------------------------------------------------------------------

Music LoadMusicStream(const char* fileName)
{
    Music music{};
    if (fileName == nullptr || !g.engineReady) return music;

    ma_sound* node = new ma_sound{};
    if (ma_sound_init_from_file(&g.engine, fileName, MA_SOUND_FLAG_STREAM, nullptr, nullptr,
                                node) != MA_SUCCESS)
    {
        delete node;
        return music;
    }

    ma_uint64 length = 0;
    ma_sound_get_length_in_pcm_frames(node, &length);
    music.frameCount = length > 0 ? static_cast<unsigned int>(length) : kDummyFrameCount;
    music.stream.internal = node;
    // raylib 的 Music 默认循环播放
    music.looping = true;
    ma_sound_set_looping(node, MA_TRUE);
    return music;
}

void UnloadMusicStream(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_uninit(node);
    delete node;
}

void PlayMusicStream(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_start(node);
}

void StopMusicStream(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_stop(node);
    ma_sound_seek_to_pcm_frame(node, 0);
}

void PauseMusicStream(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_stop(node);
}

void ResumeMusicStream(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_start(node);
}

bool IsMusicStreamPlaying(Music music)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return false;
    return ma_sound_is_playing(node) == MA_TRUE;
}

void UpdateMusicStream(Music /*music*/)
{
    // miniaudio 的流式播放由内部线程驱动，不需要每帧喂数据。
    // 保留这个函数是为了让调用点（raylib 风格的主循环）不用改。
}

void SetMusicVolume(Music music, float volume)
{
    ma_sound* node = nodeOf(music.stream);
    if (node == nullptr) return;
    ma_sound_set_volume(node, volume < 0.0f ? 0.0f : volume);
}
