#include "gl_api.h"

#include <GLFW/glfw3.h>

// 说明：GLFW 默认会在自己的头里包含 GL 头，这里已经把 gl_api.h 的 GL/gl.h 放在了
// 前面，GLFW 会检测到并从 GLFW_INCLUDE_NONE 的等价路径跳过，不会重复定义。
// glfwGetProcAddress 在各平台上分别走 glXGetProcAddress / wglGetProcAddress /
// eglGetProcAddress，正好是我们需要的。

PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;
PFNGLBUFFERSUBDATAPROC glBufferSubData = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;

PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation = nullptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLUNIFORM1IPROC glUniform1i = nullptr;
PFNGLUNIFORM1IVPROC glUniform1iv = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM2FVPROC glUniform2fv = nullptr;
PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
PFNGLUNIFORM4FVPROC glUniform4fv = nullptr;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = nullptr;

PFNGLGENERATEMIPMAPPROC glGenerateMipmap = nullptr;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
PFNGLACTIVETEXTUREPROC gfxActiveTexture = nullptr;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;

namespace
{
// 取一个入口地址；返回 nullptr 表示当前驱动没有这个函数
template <typename T>
bool load(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(glfwGetProcAddress(name));
    return fn != nullptr;
}
}

const char* gfxLoadOpenGL()
{
    // 用一个宏把所有函数过一遍，缺哪个就报哪个名字
#define GFX_LOAD(fn)                                  \
    do                                                \
    {                                                 \
        if (!load(fn, #fn)) return #fn;               \
    } while (0)

    GFX_LOAD(glGenBuffers);
    GFX_LOAD(glBindBuffer);
    GFX_LOAD(glBufferData);
    GFX_LOAD(glBufferSubData);
    GFX_LOAD(glDeleteBuffers);
    GFX_LOAD(glGenVertexArrays);
    GFX_LOAD(glBindVertexArray);
    GFX_LOAD(glDeleteVertexArrays);
    GFX_LOAD(glEnableVertexAttribArray);
    GFX_LOAD(glDisableVertexAttribArray);
    GFX_LOAD(glVertexAttribPointer);

    GFX_LOAD(glCreateShader);
    GFX_LOAD(glShaderSource);
    GFX_LOAD(glCompileShader);
    GFX_LOAD(glGetShaderiv);
    GFX_LOAD(glGetShaderInfoLog);
    GFX_LOAD(glCreateProgram);
    GFX_LOAD(glAttachShader);
    GFX_LOAD(glLinkProgram);
    GFX_LOAD(glGetProgramiv);
    GFX_LOAD(glGetProgramInfoLog);
    GFX_LOAD(glUseProgram);
    GFX_LOAD(glDeleteShader);
    GFX_LOAD(glDeleteProgram);
    GFX_LOAD(glBindAttribLocation);
    GFX_LOAD(glGetUniformLocation);
    GFX_LOAD(glUniform1i);
    GFX_LOAD(glUniform1iv);
    GFX_LOAD(glUniform1f);
    GFX_LOAD(glUniform2fv);
    GFX_LOAD(glUniform3fv);
    GFX_LOAD(glUniform4fv);
    GFX_LOAD(glUniformMatrix4fv);

    GFX_LOAD(glGenerateMipmap);
    GFX_LOAD(glGenRenderbuffers);
    GFX_LOAD(glBindRenderbuffer);
    GFX_LOAD(glRenderbufferStorageMultisample);
    GFX_LOAD(glFramebufferRenderbuffer);
    GFX_LOAD(glDeleteRenderbuffers);
    GFX_LOAD(glBlitFramebuffer);
    // 注意：变量名和 GL 入口名不一样，不能用 GFX_LOAD（它按变量名去查 glfwGetProcAddress）
    if (!load(gfxActiveTexture, "glActiveTexture")) return "glActiveTexture";
    GFX_LOAD(glGenFramebuffers);
    GFX_LOAD(glBindFramebuffer);
    GFX_LOAD(glFramebufferTexture2D);
    GFX_LOAD(glCheckFramebufferStatus);
    GFX_LOAD(glDeleteFramebuffers);

#undef GFX_LOAD

    return nullptr;
}
