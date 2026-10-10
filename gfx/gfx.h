#pragma once

// ---------------------------------------------------------------------------
// gfx —— 本项目自建的渲染 / 窗口 / 输入 / 音频 / 字体 API
//
// 实现分层（都在 gfx/ 下）：
//   window.cpp  GLFW 窗口、GL 3.3 Core 上下文、输入状态、帧计时、全屏
//   batch.cpp   2D 批渲染器：形状、贴图、离屏渲染目标、裁剪、着色器、矩阵栈
//   image.cpp   stb_image 图片解码 / 缩放 / 模糊、纹理上传、mipmap、截图导出
//   text.cpp    stb_truetype 字形图集、MeasureTextEx / DrawTextEx
//   audio.cpp   miniaudio 音频设备、Sound / Music
//   gl_api.cpp  OpenGL 3.3 函数指针加载（GL 1.1 之外的入口）
//
// 用法：`#include "gfx/gfx.h"`，然后像用 raylib 一样调用即可。
// ---------------------------------------------------------------------------

#include "types.h"

// ---- 窗口 / 生命周期 -------------------------------------------------------

void SetConfigFlags(unsigned int flags);          // 建窗口**之前**调用
void InitWindow(int width, int height, const char* title);
bool IsWindowReady();
void CloseWindow();
bool WindowShouldClose();                          // 点关闭按钮 / 退出键

void SetWindowMinSize(int width, int height);
void SetWindowState(unsigned int flags);           // 目前只支持全屏标志
void ClearWindowState(unsigned int flags);
bool IsWindowState(unsigned int flags);
void ToggleFullscreen();
void SetExitKey(int key);                          // 默认 ESC 退出，传 KEY_NULL 关掉
void SetTargetFPS(int fps);                        // <= 0 表示不限帧

int GetScreenWidth();                              // 逻辑窗口尺寸（HiDPI 下小于帧缓冲）
int GetScreenHeight();
int GetRenderWidth();                              // 帧缓冲像素尺寸（HiDPI 下更大）
int GetRenderHeight();
float GetFrameTime();                              // 上一帧耗时（秒）
void TakeScreenshot(const char* fileName);         // 当前帧存成 PNG

// ---- 输入 ------------------------------------------------------------------

bool IsKeyDown(int key);
bool IsKeyPressed(int key);
bool IsKeyReleased(int key);

bool IsMouseButtonDown(int button);
bool IsMouseButtonPressed(int button);
bool IsMouseButtonReleased(int button);
Vector2 GetMousePosition();                        // 已经过 offset/scale 映射
int GetMouseX();
float GetMouseWheelMove();
Vector2 GetMouseWheelMoveV();
void SetMouseOffset(int offsetX, int offsetY);     // GetMousePosition 的映射参数
void SetMouseScale(float scaleX, float scaleY);

bool FileExists(const char* fileName);
int GetRandomValue(int min, int max);

// ---- 帧与清屏 --------------------------------------------------------------

void BeginDrawing();
void EndDrawing();
void ClearBackground(Color color);

// ---- 离屏渲染目标 ----------------------------------------------------------

RenderTexture2D LoadRenderTexture(int width, int height);
// 多采样离屏目标：绘制走 4x(或 samples 指定) 的多采样缓冲，
// EndTextureMode() 时自动解析到 texture —— 几何边缘的全局抗锯齿就靠它。
// samples <= 1 或者驱动不支持时，自动退回普通目标（功能不变）。
RenderTexture2D LoadRenderTextureMSAA(int width, int height, int samples);
void UnloadRenderTexture(RenderTexture2D target);
void BeginTextureMode(RenderTexture2D target);
void EndTextureMode();

// ---- 裁剪 / 着色器 / 矩阵栈 -------------------------------------------------

// 混合模式：BEGIN/END 之间画的图形按 mode 混合（BLEND_ADDITIVE 用来叠加发光）
void BeginBlendMode(int mode);
void EndBlendMode();

// 画纯色形状用的 1x1 白贴图（自定义着色器画全屏/矩形时可以直接拿它当纹理）
Texture2D GetShapesTexture();

// 注意：裁剪框是「当前渲染目标像素」，**不受**矩阵栈影响（与 raylib 一致），
// 所以场景里自己乘了 renderScale 的那些调用点不需要改。
void BeginScissorMode(int x, int y, int width, int height);
void EndScissorMode();

Shader LoadShaderFromMemory(const char* vsCode, const char* fsCode);
void UnloadShader(Shader shader);
int GetShaderLocation(Shader shader, const char* uniformName);
void SetShaderValue(Shader shader, int locIndex, const void* value, int uniformType);
void BeginShaderMode(Shader shader);
void EndShaderMode();

// 2D 变换栈：只支持平移与缩放（界面不需要旋转 / 透视）
void gfxPushMatrix();
void gfxPopMatrix();
void gfxTranslatef(float x, float y, float z);
void gfxScalef(float x, float y, float z);
// 把当前批次立即提交（截图前必须调用，否则这一帧的绘制还在 CPU 缓冲里）
void gfxFlushBatch();

// ---- 形状 ------------------------------------------------------------------

void DrawRectangle(int posX, int posY, int width, int height, Color color);
void DrawRectangleRec(Rectangle rec, Color color);
void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color);
void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments,
                                 float lineThick, Color color);
void DrawRectangleGradientV(int posX, int posY, int width, int height,
                            Color top, Color bottom);
void DrawCircleV(Vector2 center, float radius, Color color);
void DrawEllipse(int centerX, int centerY, float radiusH, float radiusV, Color color);
void DrawLineEx(Vector2 startPos, Vector2 endPos, float thick, Color color);
void DrawTriangleFan(const Vector2* points, int pointCount, Color color);

bool CheckCollisionPointRec(Vector2 point, Rectangle rec);
bool CheckCollisionRecs(Rectangle rec1, Rectangle rec2);

// ---- 贴图 / 图像 -----------------------------------------------------------

Texture2D LoadTexture(const char* fileName);
Texture2D LoadTextureFromImage(Image image);
void UnloadTexture(Texture2D texture);
void UpdateTexture(Texture2D texture, const void* pixels);
void SetTextureFilter(Texture2D texture, int filter);
void SetTextureWrap(Texture2D texture, int wrap);
void GenTextureMipmaps(Texture2D* texture);
void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest,
                    Vector2 origin, float rotation, Color tint);

Image LoadImage(const char* fileName);
Image LoadImageAnim(const char* fileName, int* frames);   // GIF：所有帧竖着排在一张图上
Image ImageFromImage(Image image, Rectangle rec);
Image LoadImageFromScreen();                              // 读当前帧缓冲（自检截图用）
void UnloadImage(Image image);
void ImageResize(Image* image, int newWidth, int newHeight);
void ImageBlurGaussian(Image* image, int blurSize);
Color* LoadImageColors(Image image);
void UnloadImageColors(Color* colors);
bool ExportImage(Image image, const char* fileName);

// ---- 字体 ------------------------------------------------------------------

Font GetFontDefault();
Font LoadFontEx(const char* fileName, int fontSize, int* codepoints, int codepointCount);
void UnloadFont(Font font);
Vector2 MeasureTextEx(Font font, const char* text, float fontSize, float spacing);
void DrawTextEx(Font font, const char* text, Vector2 position, float fontSize,
                float spacing, Color tint);

// ---- 音频 ------------------------------------------------------------------

void InitAudioDevice();
void CloseAudioDevice();
bool IsAudioDeviceReady();
void SetMasterVolume(float volume);

Sound LoadSound(const char* fileName);
void UnloadSound(Sound sound);
void PlaySound(Sound sound);
void SetSoundVolume(Sound sound, float volume);
bool IsSoundPlaying(Sound sound);

Music LoadMusicStream(const char* fileName);
void UnloadMusicStream(Music music);
void PlayMusicStream(Music music);
void StopMusicStream(Music music);
void PauseMusicStream(Music music);
void ResumeMusicStream(Music music);
bool IsMusicStreamPlaying(Music music);
void UpdateMusicStream(Music music);
void SetMusicVolume(Music music, float volume);

// ---- 调试 ------------------------------------------------------------------

// 初始化 OpenGL 函数指针，返回失败原因（成功返回 nullptr）。
// 只有在 GL 上下文已经 make current 之后调用才有意义，一般由 InitWindow 内部调用。
const char* gfxLoadOpenGL();
