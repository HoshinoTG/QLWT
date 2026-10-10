#pragma once

// ---------------------------------------------------------------------------
// OpenGL 3.3 函数指针
//
// GL 1.1 的入口（glClear / glTexImage2D / glDrawArrays …）由系统 GL 库直接导出，
// 在 Linux 上是 libGL、在 Windows 上是 opengl32.dll，链接即可，不用加载。
// GL 2.0 之后的入口（VBO / VAO / 着色器 / FBO / mipmap）必须运行时取地址：
//   * Linux  : libGL 经 glXGetProcAddress 拿（GLFW 会代劳）
//   * Windows: wglGetProcAddress（GLFW 同样代劳）
// 所以这里只声明"需要加载"的那些，其余直接用 <GL/gl.h> 的原型。
//
// 刻意**不用**需要额外加载的 GL 1.3/1.4 入口：
//   * glActiveTexture —— 始终用 0 号纹理单元，不需要切换
//   * glBlendFuncSeparate —— 用 glBlendFunc 的 SRC_ALPHA 混合，和 raylib 默认一致
// ---------------------------------------------------------------------------

#include <GL/gl.h>
#include <GL/glext.h>

// Windows：winduser.h / winmm.h 里那些和绘制、声音撞名的宏要清掉，
// 否则下面的 LoadImage / PlaySound / DrawTextEx 会被替换成 ...A 版本。
#if defined(_WIN32)
    #undef LoadImage
    #undef PlaySound
    #undef DrawText
    #undef DrawTextEx
    #undef CloseWindow
    #undef ShowCursor
#endif

// 加载全部函数指针；返回 nullptr 表示成功，否则返回缺失的函数名。
// 调用前必须已经创建并 make current 一个 GL 上下文。
const char* gfxLoadOpenGL();

// ---- 顶点缓冲 / 顶点数组 ---------------------------------------------------

extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLBUFFERSUBDATAPROC glBufferSubData;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;
extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;

// ---- 着色器 ----------------------------------------------------------------

extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram;
extern PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM1IVPROC glUniform1iv;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM2FVPROC glUniform2fv;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORM4FVPROC glUniform4fv;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;

// ---- 纹理 / 帧缓冲 ---------------------------------------------------------

extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
// glActiveTexture 是 GL 1.3 的入口：Linux 的 GL/gl.h 会声明它、mingw 的只到 GL 1.1，
// 所以这里用**不同的名字**加载，避免和系统头冲突（不然 Windows 编不过）
extern PFNGLACTIVETEXTUREPROC gfxActiveTexture;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
