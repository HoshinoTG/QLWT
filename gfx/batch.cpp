#include "gfx.h"

#include "gfx_internal.h"
#include "gl_api.h"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 2D 批渲染器
//
// 和 raylib 的 rlgl 一样：所有绘制先攒进一条顶点缓冲，遇到"状态切换"
// （换贴图 / 换着色器 / 换渲染目标 / 改裁剪 / 改变换）才真正提交一次 glDrawArrays。
// 顶点里只有位置(3) + 纹理坐标(2) + 顶点色(4)，变换通过 `mvp` uniform 下发，
// 这样界面的平移/缩放不用在 CPU 上重算顶点，也保证自定义着色器拿到的
// vertexPosition 语义和 raylib 一致。
//
// 坐标约定：
//   * 默认帧缓冲：绘制坐标 = **逻辑窗口尺寸**（HiDPI 下比帧缓冲小，投影负责放大）
//   * BeginTextureMode 内：绘制坐标 = 渲染目标像素
//   * 投影一律「左上角为 (0,0)、y 向下」，和 raylib 相同
// ---------------------------------------------------------------------------

namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kSmoothCircleErrorRate = 0.5f;

// ---- 顶点 ------------------------------------------------------------------
// 布局：position(3 float) + texcoord(2 float) + color(4 ubyte)，交错存放
struct Vertex
{
    float x, y, z;
    float u, v;
    unsigned char r, g, b, a;
};

constexpr int kMaxVertices = 32768;   // 约 768KB，一次提交够画几千个三角形

// ---- 着色器状态 ------------------------------------------------------------
struct UniformSlot
{
    int location = -1;
    int type = SHADER_UNIFORM_FLOAT;
    int count = 1;
    float f[16] = {};
    int i[16] = {};
};

struct ShaderInfo
{
    int locMvp = -1;
    int locColDiffuse = -1;
    int locTex0 = -1;
    std::vector<UniformSlot> uniforms;
};

struct Gfx
{
    // GL 对象
    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int whiteTex = 0;          // 1x1 白：画纯色形状时当纹理
    Shader defaultShader{};

    // 当前批次
    std::vector<Vertex> verts;
    unsigned int curTexture = 0;
    unsigned int curProgram = 0;
    int curBlend = BLEND_ALPHA;

    // 渲染目标
    unsigned int curFbo = 0;
    unsigned int curResolveFbo = 0;   // 非 0 表示当前目标是多采样的，结束时需要解析
    int curFboW = 0;
    int curFboH = 0;
    float projW = 1.0f;               // 当前投影覆盖的逻辑宽高（绘制坐标的范围）
    float projH = 1.0f;
    unsigned int nonGlassDraws = 0;   // 非玻璃绘制的提交次数（实时玻璃按层重抓要用）
    // 裁剪状态（快照要一起存，玻璃抓取会把 scissor 关掉）
    bool scissorOn = false;
    int scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;

    // 变换（列主序，见 types.h 的 Matrix 内存布局）
    Matrix transform{};
    std::vector<Matrix> stack;

    // 投影：默认帧缓冲按逻辑窗口算，离屏按目标像素算
    Matrix projection{};

    bool ready = false;
    bool inFrame = false;

    std::unordered_map<unsigned int, ShaderInfo> shaders;
};

Gfx g;

// ---- 矩阵工具（列主序：mem[col*4 + row]）----------------------------------
inline float* mem(Matrix& m) { return &m.m0; }
inline const float* mem(const Matrix& m) { return &m.m0; }

Matrix identityMatrix()
{
    Matrix m{};
    mem(m)[0] = 1.0f;
    mem(m)[5] = 1.0f;
    mem(m)[10] = 1.0f;
    mem(m)[15] = 1.0f;
    return m;
}

// 左上角为原点、y 向下的正交投影：x∈[0,w] -> [-1,1]，y∈[0,h] -> [1,-1]
Matrix orthoTopLeft(float w, float h)
{
    Matrix m{};
    mem(m)[0] = 2.0f / w;
    mem(m)[5] = -2.0f / h;
    mem(m)[10] = -1.0f;
    mem(m)[12] = -1.0f;
    mem(m)[13] = 1.0f;
    mem(m)[15] = 1.0f;
    return m;
}

Matrix matrixMultiply(const Matrix& a, const Matrix& b)
{
    Matrix r{};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += mem(a)[k * 4 + row] * mem(b)[col * 4 + k];
            mem(r)[col * 4 + row] = sum;
        }
    }
    return r;
}

// 只含平移 + 缩放的 2D 变换
Matrix makeTransform(float tx, float ty, float sx, float sy)
{
    Matrix m{};
    mem(m)[0] = sx;
    mem(m)[5] = sy;
    mem(m)[10] = 1.0f;
    mem(m)[12] = tx;
    mem(m)[13] = ty;
    mem(m)[15] = 1.0f;
    return m;
}

void transformValues(const Matrix& m, float* tx, float* ty, float* sx, float* sy)
{
    *sx = mem(m)[0];
    *sy = mem(m)[5];
    *tx = mem(m)[12];
    *ty = mem(m)[13];
}

// ---- 着色器编译 ------------------------------------------------------------

unsigned int compileStage(unsigned int type, const char* src, const char* what)
{
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[gfx] %s 着色器编译失败：\n%s\n", what, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

const char* kDefaultVert = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

const char* kDefaultFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    finalColor = texelColor*colDiffuse*fragColor;
}
)";

// 把 mvp / colDiffuse / texture0 的位置找出来（自定义着色器里可能没有某一个）
ShaderInfo& shaderInfo(unsigned int program)
{
    ShaderInfo& info = g.shaders[program];
    if (info.locMvp == -1) info.locMvp = glGetUniformLocation(program, "mvp");
    if (info.locColDiffuse == -1) info.locColDiffuse = glGetUniformLocation(program, "colDiffuse");
    if (info.locTex0 == -1) info.locTex0 = glGetUniformLocation(program, "texture0");
    return info;
}

// ---- 批次提交 --------------------------------------------------------------

void applyUniforms(unsigned int program, const Matrix& mvp)
{
    ShaderInfo& info = shaderInfo(program);
    if (info.locMvp >= 0) glUniformMatrix4fv(info.locMvp, 1, GL_FALSE, mem(mvp));
    if (info.locColDiffuse >= 0)
    {
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        glUniform4fv(info.locColDiffuse, 1, white);
    }
    if (info.locTex0 >= 0) glUniform1i(info.locTex0, 0);   // 主纹理固定在 0 号单元

    // 用户通过 SetShaderValue 设过的 uniform：每次绑定程序都重放一遍
    for (const UniformSlot& slot : info.uniforms)
    {
        if (slot.location < 0) continue;
        switch (slot.type)
        {
        case SHADER_UNIFORM_FLOAT: glUniform1f(slot.location, slot.f[0]); break;
        case SHADER_UNIFORM_VEC2: glUniform2fv(slot.location, 1, slot.f); break;
        case SHADER_UNIFORM_VEC3: glUniform3fv(slot.location, 1, slot.f); break;
        case SHADER_UNIFORM_VEC4: glUniform4fv(slot.location, 1, slot.f); break;
        case SHADER_UNIFORM_INT: glUniform1iv(slot.location, 1, slot.i); break;
        case SHADER_UNIFORM_IVEC2: glUniform1iv(slot.location, 2, slot.i); break;
        case SHADER_UNIFORM_IVEC3: glUniform1iv(slot.location, 3, slot.i); break;
        case SHADER_UNIFORM_IVEC4: glUniform1iv(slot.location, 4, slot.i); break;
        case SHADER_UNIFORM_SAMPLER2D: glUniform1i(slot.location, slot.i[0]); break;
        default: break;
        }
    }
}

void submitBatch()
{
    if (g.verts.empty()) return;
    // 非玻璃的绘制会让"当前画面"变样，已抓的玻璃源要作废（见 gfx_internal.h 的说明）
    if (!gfxdetail::gGlassDrawing) ++g.nonGlassDraws;

    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<long>(g.verts.size() * sizeof(Vertex)), g.verts.data());

    const unsigned int program = g.curProgram != 0 ? g.curProgram : g.defaultShader.id;
    glUseProgram(program);

    // 视口的宽高决定投影；mvp = 投影 × 变换（变换作用在绘制坐标上）
    Matrix mvp = matrixMultiply(g.projection, g.transform);
    applyUniforms(program, mvp);

    // 混合模式（发光叠加要用 BLEND_ADDITIVE，见 post.cpp 的泛光）
    switch (g.curBlend)
    {
    case BLEND_ADDITIVE: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
    case BLEND_MULTIPLIED: glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
    case BLEND_ADD_COLORS: glBlendFunc(GL_ONE, GL_ONE); break;
    case BLEND_ALPHA:
    default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    }

    // 主纹理始终用 0 号纹理单元，不需要 glActiveTexture
    glBindTexture(GL_TEXTURE_2D, g.curTexture);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(g.verts.size()));
    g.verts.clear();
}

void setBlendMode(int mode)
{
    if (g.curBlend == mode) return;
    submitBatch();
    g.curBlend = mode;
}

void setTexture(unsigned int texture)
{
    if (g.curTexture == texture) return;
    submitBatch();
    g.curTexture = texture;
}

void setProgram(unsigned int program)
{
    if (g.curProgram == program) return;
    submitBatch();
    g.curProgram = program;
}

// 顶点数量到上限就提交一次，避免超出预分配的缓冲
void ensureRoom(int count)
{
    if (static_cast<int>(g.verts.size()) + count > kMaxVertices) submitBatch();
}

void emit(float x, float y, float u, float v, Color color)
{
    Vertex vert;
    vert.x = x;
    vert.y = y;
    vert.z = 0.0f;
    vert.u = u;
    vert.v = v;
    vert.r = color.r;
    vert.g = color.g;
    vert.b = color.b;
    vert.a = color.a;
    g.verts.push_back(vert);
}

// 三角形（顶点色相同）
void emitTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c)
{
    ensureRoom(3);
    emit(x0, y0, 0.0f, 0.0f, c);
    emit(x1, y1, 0.0f, 0.0f, c);
    emit(x2, y2, 0.0f, 0.0f, c);
}

// 四边形：p0..p3 依次为左上、左下、右下、右上（与 raylib 的 QUADS 顺序一致）
void emitQuad(Texture2D texture, Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3,
              Vector2 uv0, Vector2 uv1, Vector2 uv2, Vector2 uv3, Color c)
{
    setTexture(texture.id);
    ensureRoom(6);
    emit(p0.x, p0.y, uv0.x, uv0.y, c);
    emit(p1.x, p1.y, uv1.x, uv1.y, c);
    emit(p2.x, p2.y, uv2.x, uv2.y, c);

    emit(p0.x, p0.y, uv0.x, uv0.y, c);
    emit(p2.x, p2.y, uv2.x, uv2.y, c);
    emit(p3.x, p3.y, uv3.x, uv3.y, c);
}

// 纯色四边形：用 1x1 白贴图，纹理坐标随便取
void emitColorQuad(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, Color c)
{
    Texture2D white = gfxdetail::whiteTexture();
    emitQuad(white, p0, p1, p2, p3, {0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, c);
}

void emitColorTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c)
{
    setTexture(gfxdetail::whiteTexture().id);
    emitTriangle(x0, y0, x1, y1, x2, y2, c);
}

// 圆角半径（像素）-> raylib 的 roundness 比例
// 按 raylib 的规则算圆角段数（segments < 4 时按误差率推）
int cornerSegments(int segments, float radius, float factor)
{
    if (segments >= 4) return segments;
    float th = std::acos(2.0f * std::pow(1.0f - kSmoothCircleErrorRate / radius, 2.0f) - 1.0f);
    int count = static_cast<int>(std::ceil(2.0f * kPi / th) / factor);
    return count <= 0 ? 4 : count;
}
}   // namespace

// ---------------------------------------------------------------------------
// 内部接口
// ---------------------------------------------------------------------------

namespace gfxdetail
{
bool gGlassDrawing = false;   // 渲染器正在画玻璃（gfx 之外设置）
void setGlassDrawInProgress(bool inProgress) { gGlassDrawing = inProgress; }
unsigned int nonGlassDrawCount() { return g.nonGlassDraws; }

Texture2D whiteTexture()
{
    Texture2D tex{};
    tex.id = g.whiteTex;
    tex.width = 1;
    tex.height = 1;
    tex.mipmaps = 1;
    tex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return tex;
}

void flushBatch() { submitBatch(); }

void saveTargetState(gfxdetail::TargetState* out)
{
    if (!out) return;
    out->fbo = g.curFbo;
    out->resolveFbo = g.curResolveFbo;
    out->width = g.curFboW;
    out->height = g.curFboH;
    out->projW = g.projW;
    out->projH = g.projH;
    out->projection = g.projection;
    out->transform = g.transform;
    out->scissor = g.scissorOn;
    out->scissorX = g.scissorX;
    out->scissorY = g.scissorY;
    out->scissorW = g.scissorW;
    out->scissorH = g.scissorH;
}

void restoreTargetState(const gfxdetail::TargetState& state)
{
    gfxdetail::flushBatch();
    glBindFramebuffer(GL_FRAMEBUFFER, state.fbo);
    glViewport(0, 0, state.width, state.height);
    if (state.scissor)
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(state.scissorX, state.scissorY, state.scissorW, state.scissorH);
    }
    else
    {
        glDisable(GL_SCISSOR_TEST);
    }
    g.curFbo = state.fbo;
    g.curResolveFbo = state.resolveFbo;
    g.curFboW = state.width;
    g.curFboH = state.height;
    g.projW = state.projW;
    g.projH = state.projH;
    g.projection = state.projection;
    g.transform = state.transform;
    g.scissorOn = state.scissor;
    g.scissorX = state.scissorX;
    g.scissorY = state.scissorY;
    g.scissorW = state.scissorW;
    g.scissorH = state.scissorH;
}

unsigned int currentFramebuffer() { return g.curFbo; }
bool currentTargetMultisampled() { return g.curResolveFbo != 0; }
void currentProjectionSize(float* width, float* height)
{
    if (width) *width = g.projW;
    if (height) *height = g.projH;
}

int currentRenderTargetWidth() { return g.curFboW; }

int currentRenderTargetHeight() { return g.curFboH; }

void initBatch()
{
    if (g.ready) return;

    g.verts.reserve(kMaxVertices);
    g.transform = identityMatrix();
    g.projection = orthoTopLeft(1.0f, 1.0f);

    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, kMaxVertices * static_cast<long>(sizeof(Vertex)),
                 nullptr, GL_DYNAMIC_DRAW);

    // 交错顶点布局：0=位置 1=纹理坐标 2=顶点色
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);

    // 1x1 白贴图
    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g.whiteTex);
    glBindTexture(GL_TEXTURE_2D, g.whiteTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    g.defaultShader = LoadShaderFromMemory(kDefaultVert, kDefaultFrag);
    g.ready = true;
}
}   // namespace gfxdetail

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------

void BeginDrawing()
{
    gfxdetail::beginFrameTiming();

    int fbW = 0, fbH = 0;
    gfxdetail::framebufferSize(&fbW, &fbH);
    if (fbW <= 0 || fbH <= 0) return;

    // 默认帧缓冲：视口铺满帧缓冲像素，投影按逻辑窗口尺寸（HiDPI 自动放大）
    gfxdetail::flushBatch();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbW, fbH);
    glDisable(GL_SCISSOR_TEST);

    g.curFbo = 0;
    g.curResolveFbo = 0;
    g.curFboW = fbW;
    g.curFboH = fbH;
    // 窗口被最小化时逻辑尺寸可能是 0，这时退回帧缓冲尺寸，避免投影除零
    const int screenW = GetScreenWidth() > 0 ? GetScreenWidth() : fbW;
    const int screenH = GetScreenHeight() > 0 ? GetScreenHeight() : fbH;
    g.projection = orthoTopLeft(static_cast<float>(screenW), static_cast<float>(screenH));
    g.projW = static_cast<float>(screenW);
    g.projH = static_cast<float>(screenH);
    g.transform = identityMatrix();
    g.stack.clear();
    g.inFrame = true;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    g.curBlend = BLEND_ALPHA;   // 防止上一帧忘了 EndBlendMode
}

void EndDrawing()
{
    gfxdetail::flushBatch();
    g.inFrame = false;
    gfxdetail::swapAndEndFrame();
}

void ClearBackground(Color color)
{
    gfxdetail::flushBatch();
    glClearColor(static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                 static_cast<float>(color.b) / 255.0f, static_cast<float>(color.a) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// ---------------------------------------------------------------------------
// 离屏渲染目标
// ---------------------------------------------------------------------------

namespace
{
// 建一张 RGBA8 颜色贴图（离屏目标的颜色附件）
void createColorTexture(Texture2D* texture, int width, int height)
{
    glGenTextures(1, &texture->id);
    glBindTexture(GL_TEXTURE_2D, texture->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // 离屏目标默认双线性（和 raylib 一致，调用方还会再设一次）
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texture->width = width;
    texture->height = height;
    texture->mipmaps = 1;
    texture->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
}
}   // namespace

RenderTexture2D LoadRenderTexture(int width, int height)
{
    RenderTexture2D target{};
    if (width <= 0 || height <= 0) return target;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    createColorTexture(&target.texture, width, height);

    glGenFramebuffers(1, &target.id);
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target.texture.id, 0);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        std::fprintf(stderr, "[gfx] 渲染目标创建失败（%dx%d，状态 0x%X）\n", width, height, status);
        glDeleteFramebuffers(1, &target.id);
        glDeleteTextures(1, &target.texture.id);
        return RenderTexture2D{};
    }

    // 深度附件：本项目是纯 2D，不需要深度缓冲，省一块显存
    return target;
}

RenderTexture2D LoadRenderTextureMSAA(int width, int height, int samples)
{
    if (samples <= 1) return LoadRenderTexture(width, height);
    if (width <= 0 || height <= 0) return RenderTexture2D{};

    // 驱动支持的最大采样数（拿不到就按 4 处理）
    GLint maxSamples = 4;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    if (maxSamples < 2)
    {
        std::fprintf(stderr, "[gfx] 该驱动不支持多重采样，抗锯齿退回普通目标\n");
        return LoadRenderTexture(width, height);
    }
    const int useSamples = samples > maxSamples ? maxSamples : samples;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    RenderTexture2D target{};
    createColorTexture(&target.texture, width, height);

    // 解析目标：普通 FBO，颜色附件是上面这张贴图
    glGenFramebuffers(1, &target.resolveFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target.resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target.texture.id, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    // 绘制目标：多采样 renderbuffer
    glGenRenderbuffers(1, &target.msaaColor);
    glBindRenderbuffer(GL_RENDERBUFFER, target.msaaColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, useSamples, GL_RGBA8, width, height);

    glGenFramebuffers(1, &target.id);
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              target.msaaColor);
    const GLenum msaaStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));

    if (status != GL_FRAMEBUFFER_COMPLETE || msaaStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        std::fprintf(stderr, "[gfx] 多重采样渲染目标创建失败（%dx%d x%d，0x%X / 0x%X），退回普通目标\n",
                     width, height, useSamples, status, msaaStatus);
        if (target.id) glDeleteFramebuffers(1, &target.id);
        if (target.resolveFbo) glDeleteFramebuffers(1, &target.resolveFbo);
        if (target.msaaColor) glDeleteRenderbuffers(1, &target.msaaColor);
        if (target.texture.id) glDeleteTextures(1, &target.texture.id);
        return LoadRenderTexture(width, height);
    }

    target.samples = useSamples;
    std::printf("[gfx] 离屏多重采样已开启：%dx%d x%d\n", width, height, useSamples);
    return target;
}

void UnloadRenderTexture(RenderTexture2D target)
{
    if (target.id != 0) glDeleteFramebuffers(1, &target.id);
    if (target.resolveFbo != 0) glDeleteFramebuffers(1, &target.resolveFbo);
    if (target.msaaColor != 0) glDeleteRenderbuffers(1, &target.msaaColor);
    if (target.texture.id != 0) glDeleteTextures(1, &target.texture.id);
}

void BeginTextureMode(RenderTexture2D target)
{
    if (target.id == 0) return;
    gfxdetail::flushBatch();

    // 重置裁剪：目标换了以后原来的裁剪框没有意义（raylib 同样不换算）
    glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    glViewport(0, 0, target.texture.width, target.texture.height);

    g.curFbo = target.id;
    g.curResolveFbo = target.resolveFbo;   // 非 0：结束时要把多采样结果解析出来
    g.curFboW = target.texture.width;
    g.curFboH = target.texture.height;
    g.projection = orthoTopLeft(static_cast<float>(target.texture.width),
                                static_cast<float>(target.texture.height));
    g.projW = static_cast<float>(target.texture.width);
    g.projH = static_cast<float>(target.texture.height);
    g.transform = identityMatrix();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    g.curBlend = BLEND_ALPHA;   // 防止上一帧忘了 EndBlendMode
}

void EndTextureMode()
{
    gfxdetail::flushBatch();
    glDisable(GL_SCISSOR_TEST);

    // 多采样目标：blit 到普通 FBO，把 4 个采样平均成 1 个像素（这一步就是抗锯齿）
    if (g.curResolveFbo != 0)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.curFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.curResolveFbo);
        glBlitFramebuffer(0, 0, g.curFboW, g.curFboH,
                          0, 0, g.curFboW, g.curFboH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g.curResolveFbo = 0;
    }

    int fbW = 0, fbH = 0;
    gfxdetail::framebufferSize(&fbW, &fbH);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbW, fbH);

    g.curFbo = 0;
    g.curFboW = fbW;
    g.curFboH = fbH;
    g.projection = orthoTopLeft(static_cast<float>(GetScreenWidth()),
                                static_cast<float>(GetScreenHeight()));
    g.transform = identityMatrix();
}

// ---------------------------------------------------------------------------
// 裁剪 / 着色器 / 变换
// ---------------------------------------------------------------------------

void BeginScissorMode(int x, int y, int width, int height)
{
    gfxdetail::flushBatch();
    glEnable(GL_SCISSOR_TEST);
    // 绘制坐标是 y 向下，GL 的裁剪框是 y 向上，按当前渲染目标的高度翻转
    const int gy = g.curFboH - (y + height);
    glScissor(x, gy, width, height);
    g.scissorOn = true;
    g.scissorX = x;
    g.scissorY = gy;
    g.scissorW = width;
    g.scissorH = height;
}

void EndScissorMode()
{
    gfxdetail::flushBatch();
    glDisable(GL_SCISSOR_TEST);
    g.scissorOn = false;
}

Shader LoadShaderFromMemory(const char* vsCode, const char* fsCode)
{
    Shader shader{};
    if (vsCode == nullptr || fsCode == nullptr) return shader;

    unsigned int vs = compileStage(GL_VERTEX_SHADER, vsCode, "顶点");
    unsigned int fs = compileStage(GL_FRAGMENT_SHADER, fsCode, "片段");
    if (vs == 0 || fs == 0)
    {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return shader;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    // 固定属性槽位：所有着色器共用同一套顶点布局，VAO 才能复用
    glBindAttribLocation(program, 0, "vertexPosition");
    glBindAttribLocation(program, 1, "vertexTexCoord");
    glBindAttribLocation(program, 2, "vertexColor");
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[gfx] 着色器链接失败：\n%s\n", log);
        glDeleteProgram(program);
        return shader;
    }

    shader.id = program;
    shaderInfo(program);   // 提前把 mvp/colDiffuse/texture0 的位置查好
    return shader;
}

void UnloadShader(Shader shader)
{
    if (shader.id == 0) return;
    if (g.curProgram == shader.id) setProgram(0);
    g.shaders.erase(shader.id);
    glDeleteProgram(shader.id);
}

int GetShaderLocation(Shader shader, const char* uniformName)
{
    if (shader.id == 0 || uniformName == nullptr) return -1;
    return glGetUniformLocation(shader.id, uniformName);
}

void SetShaderValue(Shader shader, int locIndex, const void* value, int uniformType)
{
    if (shader.id == 0 || locIndex < 0 || value == nullptr) return;

    ShaderInfo& info = shaderInfo(shader.id);
    UniformSlot* slot = nullptr;
    for (UniformSlot& candidate : info.uniforms)
    {
        if (candidate.location == locIndex)
        {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr)
    {
        info.uniforms.emplace_back();
        slot = &info.uniforms.back();
        slot->location = locIndex;
    }
    slot->type = uniformType;

    // 按类型拷贝：这里只支持 1 个元素（工程里的用法都是单值）
    switch (uniformType)
    {
    case SHADER_UNIFORM_FLOAT:
        slot->count = 1;
        std::memcpy(slot->f, value, sizeof(float));
        break;
    case SHADER_UNIFORM_VEC2:
        slot->count = 1;
        std::memcpy(slot->f, value, sizeof(float) * 2);
        break;
    case SHADER_UNIFORM_VEC3:
        slot->count = 1;
        std::memcpy(slot->f, value, sizeof(float) * 3);
        break;
    case SHADER_UNIFORM_VEC4:
        slot->count = 1;
        std::memcpy(slot->f, value, sizeof(float) * 4);
        break;
    case SHADER_UNIFORM_INT:
    case SHADER_UNIFORM_SAMPLER2D:
        slot->count = 1;
        std::memcpy(slot->i, value, sizeof(int));
        break;
    case SHADER_UNIFORM_IVEC2:
        slot->count = 2;
        std::memcpy(slot->i, value, sizeof(int) * 2);
        break;
    case SHADER_UNIFORM_IVEC3:
        slot->count = 3;
        std::memcpy(slot->i, value, sizeof(int) * 3);
        break;
    case SHADER_UNIFORM_IVEC4:
        slot->count = 4;
        std::memcpy(slot->i, value, sizeof(int) * 4);
        break;
    default:
        break;
    }
}

void BeginShaderMode(Shader shader) { setProgram(shader.id); }

void EndShaderMode() { setProgram(0); }

void BeginBlendMode(int mode) { setBlendMode(mode); }

void EndBlendMode() { setBlendMode(BLEND_ALPHA); }

Texture2D GetShapesTexture() { return gfxdetail::whiteTexture(); }

void gfxPushMatrix()
{
    gfxdetail::flushBatch();
    g.stack.push_back(g.transform);
}

void gfxPopMatrix()
{
    gfxdetail::flushBatch();
    if (!g.stack.empty())
    {
        g.transform = g.stack.back();
        g.stack.pop_back();
    }
}

void gfxTranslatef(float x, float y, float /*z*/)
{
    // 与 raylib 的 rlTranslatef 等价：新变换 = 原变换 × 平移，
    // 所以平移量要乘上当前缩放（外层 gfxScalef 的缩放会作用到平移上）
    gfxdetail::flushBatch();
    float tx = 0.0f, ty = 0.0f, sx = 1.0f, sy = 1.0f;
    transformValues(g.transform, &tx, &ty, &sx, &sy);
    g.transform = makeTransform(tx + sx * x, ty + sy * y, sx, sy);
}

void gfxScalef(float x, float y, float /*z*/)
{
    gfxdetail::flushBatch();
    float tx = 0.0f, ty = 0.0f, sx = 1.0f, sy = 1.0f;
    transformValues(g.transform, &tx, &ty, &sx, &sy);
    g.transform = makeTransform(tx, ty, sx * x, sy * y);
}

void gfxFlushBatch() { gfxdetail::flushBatch(); }

// ---------------------------------------------------------------------------
// 形状
// ---------------------------------------------------------------------------

void DrawRectangleRec(Rectangle rec, Color color)
{
    if (rec.width == 0.0f || rec.height == 0.0f) return;
    Vector2 p0{rec.x, rec.y};
    Vector2 p1{rec.x, rec.y + rec.height};
    Vector2 p2{rec.x + rec.width, rec.y + rec.height};
    Vector2 p3{rec.x + rec.width, rec.y};
    emitColorQuad(p0, p1, p2, p3, color);
}

void DrawRectangle(int posX, int posY, int width, int height, Color color)
{
    DrawRectangleRec({static_cast<float>(posX), static_cast<float>(posY),
                      static_cast<float>(width), static_cast<float>(height)},
                     color);
}

void DrawRectangleGradientV(int posX, int posY, int width, int height,
                            Color top, Color bottom)
{
    if (width == 0 || height == 0) return;
    const float x = static_cast<float>(posX);
    const float y = static_cast<float>(posY);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // 顶点色不同，用「1x1 白贴图 + 两个三角形」拼出竖直渐变
    setTexture(gfxdetail::whiteTexture().id);
    ensureRoom(6);
    emit(x, y, 0.0f, 0.0f, top);
    emit(x, y + h, 0.0f, 1.0f, bottom);
    emit(x + w, y + h, 1.0f, 1.0f, bottom);
    emit(x, y, 0.0f, 0.0f, top);
    emit(x + w, y + h, 1.0f, 1.0f, bottom);
    emit(x + w, y, 1.0f, 0.0f, top);
}

void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color)
{
    if (roundness <= 0.0f || rec.width < 1.0f || rec.height < 1.0f)
    {
        DrawRectangleRec(rec, color);
        return;
    }
    if (roundness > 1.0f) roundness = 1.0f;

    const float radius = (rec.width > rec.height ? rec.height * roundness / 2.0f
                                                 : rec.width * roundness / 2.0f);
    if (radius <= 0.0f) return;

    segments = cornerSegments(segments, radius, 4.0f);
    const float step = 90.0f / static_cast<float>(segments);

    // 12 个关键点（和 raylib 一致）：4 个角心 + 4 条直边的端点
    const Vector2 point[12] = {
        {rec.x + radius, rec.y},
        {rec.x + rec.width - radius, rec.y},
        {rec.x + rec.width, rec.y + radius},
        {rec.x + rec.width, rec.y + rec.height - radius},
        {rec.x + rec.width - radius, rec.y + rec.height},
        {rec.x + radius, rec.y + rec.height},
        {rec.x, rec.y + rec.height - radius},
        {rec.x, rec.y + radius},
        {rec.x + radius, rec.y + radius},
        {rec.x + rec.width - radius, rec.y + radius},
        {rec.x + rec.width - radius, rec.y + rec.height - radius},
        {rec.x + radius, rec.y + rec.height - radius},
    };
    const Vector2 centers[4] = {point[8], point[9], point[10], point[11]};
    const float angles[4] = {180.0f, 270.0f, 0.0f, 90.0f};

    // 四个圆角：以角心为扇心
    for (int k = 0; k < 4; ++k)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = angles[k] + step * static_cast<float>(i);
            const float a1 = a0 + step;
            emitColorTriangle(
                centers[k].x, centers[k].y,
                centers[k].x + std::cos(kDeg2Rad * a0) * radius,
                centers[k].y + std::sin(kDeg2Rad * a0) * radius,
                centers[k].x + std::cos(kDeg2Rad * a1) * radius,
                centers[k].y + std::sin(kDeg2Rad * a1) * radius, color);
        }
    }

    // 中间 + 上下左右四块矩形
    emitColorQuad(point[8], point[9], point[1], point[0], color);       // 上
    emitColorQuad(point[2], point[9], point[10], point[3], color);      // 右
    emitColorQuad(point[11], point[5], point[4], point[10], color);     // 下
    emitColorQuad(point[7], point[6], point[11], point[8], color);      // 左
    emitColorQuad(point[8], point[11], point[10], point[9], color);     // 中
}

void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments,
                                 float lineThick, Color color)
{
    if (lineThick < 0.0f) lineThick = 0.0f;
    if (lineThick <= 0.0f || color.a == 0) return;

    if (roundness <= 0.0f)
    {
        // 直角：四条边各画一个矩形（描边在外侧，和 raylib 一致）
        Rectangle outer{rec.x - lineThick, rec.y - lineThick,
                        rec.width + 2.0f * lineThick, rec.height + 2.0f * lineThick};
        DrawRectangleRec({outer.x, outer.y, outer.width, lineThick}, color);
        DrawRectangleRec({outer.x, outer.y + outer.height - lineThick, outer.width, lineThick}, color);
        DrawRectangleRec({outer.x, outer.y + lineThick, lineThick, outer.height - 2.0f * lineThick}, color);
        DrawRectangleRec({outer.x + outer.width - lineThick, outer.y + lineThick, lineThick,
                          outer.height - 2.0f * lineThick}, color);
        return;
    }
    if (roundness > 1.0f) roundness = 1.0f;

    const float radius = (rec.width > rec.height ? rec.height * roundness / 2.0f
                                                 : rec.width * roundness / 2.0f);
    if (radius <= 0.0f) return;

    segments = cornerSegments(segments, radius, 2.0f);
    const float step = 90.0f / static_cast<float>(segments);
    const float inner = radius;
    const float outer = radius + lineThick;

    const Vector2 centers[4] = {
        {rec.x + inner, rec.y + inner},
        {rec.x + rec.width - inner, rec.y + inner},
        {rec.x + rec.width - inner, rec.y + rec.height - inner},
        {rec.x + inner, rec.y + rec.height - inner},
    };
    const float angles[4] = {180.0f, 270.0f, 0.0f, 90.0f};

    // 四个圆角：内弧 - 外弧之间的环带
    for (int k = 0; k < 4; ++k)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = angles[k] + step * static_cast<float>(i);
            const float a1 = a0 + step;
            Vector2 i0{centers[k].x + std::cos(kDeg2Rad * a0) * inner,
                       centers[k].y + std::sin(kDeg2Rad * a0) * inner};
            Vector2 i1{centers[k].x + std::cos(kDeg2Rad * a1) * inner,
                       centers[k].y + std::sin(kDeg2Rad * a1) * inner};
            Vector2 o0{centers[k].x + std::cos(kDeg2Rad * a0) * outer,
                       centers[k].y + std::sin(kDeg2Rad * a0) * outer};
            Vector2 o1{centers[k].x + std::cos(kDeg2Rad * a1) * outer,
                       centers[k].y + std::sin(kDeg2Rad * a1) * outer};
            emitColorQuad(o0, i0, i1, o1, color);
        }
    }

    // 四条直边：内外两条线之间的矩形
    const float x0 = rec.x, y0 = rec.y;
    const float x1 = rec.x + rec.width, y1 = rec.y + rec.height;
    emitColorQuad({x0 + inner, y0 - lineThick}, {x0 + inner, y0},
                  {x1 - inner, y0}, {x1 - inner, y0 - lineThick}, color);          // 上
    emitColorQuad({x1, y0 + inner}, {x1, y1 - inner},
                  {x1 + lineThick, y1 - inner}, {x1 + lineThick, y0 + inner}, color);  // 右
    emitColorQuad({x1 - inner, y1 + lineThick}, {x1 - inner, y1},
                  {x0 + inner, y1}, {x0 + inner, y1 + lineThick}, color);          // 下
    emitColorQuad({x0 - lineThick, y1 - inner}, {x0 - lineThick, y0 + inner},
                  {x0, y0 + inner}, {x0, y1 - inner}, color);                      // 左
}

void DrawCircleV(Vector2 center, float radius, Color color)
{
    if (radius <= 0.0f) radius = 0.1f;
    const int segments = 36;
    const float step = 360.0f / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i)
    {
        const float a0 = step * static_cast<float>(i);
        const float a1 = a0 + step;
        emitColorTriangle(center.x, center.y,
                          center.x + std::cos(kDeg2Rad * a1) * radius,
                          center.y + std::sin(kDeg2Rad * a1) * radius,
                          center.x + std::cos(kDeg2Rad * a0) * radius,
                          center.y + std::sin(kDeg2Rad * a0) * radius, color);
    }
}

void DrawEllipse(int centerX, int centerY, float radiusH, float radiusV, Color color)
{
    // 与 raylib 相同：每 10 度一个三角形
    for (int i = 0; i < 360; i += 10)
    {
        const float cx = static_cast<float>(centerX);
        const float cy = static_cast<float>(centerY);
        emitColorTriangle(
            cx, cy,
            cx + std::cos(kDeg2Rad * static_cast<float>(i + 10)) * radiusH,
            cy + std::sin(kDeg2Rad * static_cast<float>(i + 10)) * radiusV,
            cx + std::cos(kDeg2Rad * static_cast<float>(i)) * radiusH,
            cy + std::sin(kDeg2Rad * static_cast<float>(i)) * radiusV, color);
    }
}

void DrawLineEx(Vector2 startPos, Vector2 endPos, float thick, Color color)
{
    const float dx = endPos.x - startPos.x;
    const float dy = endPos.y - startPos.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f || thick <= 0.0f) return;

    const float scale = thick / (2.0f * length);
    const Vector2 r{-scale * dy, scale * dx};
    emitColorQuad({startPos.x - r.x, startPos.y - r.y},
                  {startPos.x + r.x, startPos.y + r.y},
                  {endPos.x + r.x, endPos.y + r.y},
                  {endPos.x - r.x, endPos.y - r.y}, color);
}

void DrawTriangleFan(const Vector2* points, int pointCount, Color color)
{
    if (points == nullptr || pointCount < 3) return;
    for (int i = 1; i < pointCount - 1; ++i)
    {
        emitColorTriangle(points[0].x, points[0].y,
                          points[i].x, points[i].y,
                          points[i + 1].x, points[i + 1].y, color);
    }
}

bool CheckCollisionPointRec(Vector2 point, Rectangle rec)
{
    return (point.x >= rec.x) && (point.x < (rec.x + rec.width)) &&
           (point.y >= rec.y) && (point.y < (rec.y + rec.height));
}

bool CheckCollisionRecs(Rectangle rec1, Rectangle rec2)
{
    return (rec1.x < (rec2.x + rec2.width) && (rec1.x + rec1.width) > rec2.x) &&
           (rec1.y < (rec2.y + rec2.height) && (rec1.y + rec1.height) > rec2.y);
}

// ---------------------------------------------------------------------------
// 贴图绘制（batch.cpp 侧，纹理对象由 image.cpp 管理）
// ---------------------------------------------------------------------------

void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest,
                    Vector2 origin, float rotation, Color tint)
{
    if (texture.id == 0) return;

    const float width = static_cast<float>(texture.width);
    const float height = static_cast<float>(texture.height);

    bool flipX = false;
    if (source.width < 0.0f)
    {
        flipX = true;
        source.width *= -1.0f;
    }
    if (source.height < 0.0f) source.y -= source.height;

    Vector2 topLeft{}, topRight{}, bottomLeft{}, bottomRight{};
    if (rotation == 0.0f)
    {
        const float x = dest.x - origin.x;
        const float y = dest.y - origin.y;
        topLeft = {x, y};
        topRight = {x + dest.width, y};
        bottomLeft = {x, y + dest.height};
        bottomRight = {x + dest.width, y + dest.height};
    }
    else
    {
        const float sinR = std::sin(rotation * kDeg2Rad);
        const float cosR = std::cos(rotation * kDeg2Rad);
        const float x = dest.x;
        const float y = dest.y;
        const float dx = -origin.x;
        const float dy = -origin.y;

        topLeft = {x + dx * cosR - dy * sinR, y + dx * sinR + dy * cosR};
        topRight = {x + (dx + dest.width) * cosR - dy * sinR,
                    y + (dx + dest.width) * sinR + dy * cosR};
        bottomLeft = {x + dx * cosR - (dy + dest.height) * sinR,
                      y + dx * sinR + (dy + dest.height) * cosR};
        bottomRight = {x + (dx + dest.width) * cosR - (dy + dest.height) * sinR,
                       y + (dx + dest.width) * sinR + (dy + dest.height) * cosR};
    }

    const float u0 = source.x / width;
    const float v0 = source.y / height;
    const float u1 = (source.x + source.width) / width;
    const float v1 = (source.y + source.height) / height;

    Vector2 uvTopLeft = flipX ? Vector2{u1, v0} : Vector2{u0, v0};
    Vector2 uvBottomLeft = flipX ? Vector2{u1, v1} : Vector2{u0, v1};
    Vector2 uvBottomRight = flipX ? Vector2{u0, v1} : Vector2{u1, v1};
    Vector2 uvTopRight = flipX ? Vector2{u0, v0} : Vector2{u1, v0};

    emitQuad(texture, topLeft, bottomLeft, bottomRight, topRight,
             uvTopLeft, uvBottomLeft, uvBottomRight, uvTopRight, tint);
}

namespace gfxdetail
{
void pushQuad(Texture2D texture, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, Color tint)
{
    emitQuad(texture, {x0, y0}, {x0, y1}, {x1, y1}, {x1, y0},
             {u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}, tint);
}
}   // namespace gfxdetail
