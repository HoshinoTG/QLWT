# ===========================================================================
# QLWT 视觉小说引擎 —— Linux / macOS 构建（与 build.ps1 对应）
#
#   make            增量编译，产物 ./qlwt
#   make -j8        并行编译（快很多）
#   make clean      清理目标文件与可执行文件
#   make run        编译并启动游戏
#   make check      无头自检：脚本能跑完 / 每个 chapter 能跑完 / 存读档
#   make font       从系统字体抽取一个中文 .ttf 到 assets/fonts/（Linux/macOS 首次需要）
#   make release    打包发行版：dist/qlwt-<系统>-<架构>/ 与 .tar.gz（可整目录拷走）
#   make release-windows  交叉编译 Windows 版：dist/qlwt-windows-x86_64(.zip)
#
# 依赖：g++（C++20）+ GLFW3 + 系统 OpenGL。音频由自带的 miniaudio 提供，
#       图片 / 字体由自带的 stb 提供，都不需要额外装库。
#   Debian/Ubuntu: sudo apt install g++ libglfw3-dev libgl1-mesa-dev
#   Arch:          sudo pacman -S gcc glfw mesa
#   macOS:         brew install glfw
#
# 注意：引擎启动时会把工作目录切到可执行文件所在目录，而素材是按
# assets/... 相对路径读取的，所以 qlwt 必须和 assets/ 放在同一层
# （也就是仓库根目录，和 Windows 下的 qlwt.exe 一样）。
# ===========================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I.
LDFLAGS  ?=

UNAME_S  := $(shell uname -s)

# GLFW / OpenGL：优先用 pkg-config，拿不到就退回手工链接
GFX_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
  GFX_LIBS := $(shell pkg-config --libs glfw3 2>/dev/null)
  ifeq ($(strip $(GFX_LIBS)),)
    GFX_LIBS := -lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
  endif
else
  GFX_LIBS := $(shell pkg-config --libs glfw3 gl 2>/dev/null)
  ifeq ($(strip $(GFX_LIBS)),)
    GFX_LIBS := -lglfw -lGL
  endif
  GFX_LIBS += -lpthread -ldl -lm
endif

SRCS    := $(wildcard core/*.cpp) $(wildcard game/*.cpp) $(wildcard renderer/*.cpp) \
           $(wildcard gfx/*.cpp) main.cpp
OBJDIR  := build/obj
OBJS    := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)
TARGET  := qlwt

.PHONY: all clean run check font release release-windows help

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(GFX_LIBS)
	@echo "build OK -> ./$(TARGET)"

# ---------------------------------------------------------------------------
# 发行版打包
#
#   make release     生成 dist/qlwt-<系统>-<架构>/（整个目录拷哪都能跑）
#                    以及 dist/qlwt-<系统>-<架构>.tar.gz
#
# 和开发用的 ./qlwt 区别：
#   * rpath 改成 $ORIGIN/lib，跟着可执行文件走，不再写死到本仓库路径
#   * 把 libglfw.so.3 一起打进包（GLFW 是 zlib 许可，可以随包分发），
#     目标机器只要有 OpenGL / X11 / Wayland 这些系统库就能跑，
#     不需要装 glfw 开发包
#   * strip 掉符号，体积更小
#   * 连 assets/ 一起打包（引擎按可执行文件所在目录找素材）
# ---------------------------------------------------------------------------
DIST_NAME ?= qlwt-$(shell uname -s | tr 'A-Z' 'a-z')-$(shell uname -m)
DIST_DIR  := dist/$(DIST_NAME)

release: $(OBJS)
	@rm -rf $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/lib
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $(DIST_DIR)/qlwt $(OBJS) $(GFX_LIBS) \
	    -Wl,-rpath,'$$ORIGIN/lib'
	@strip $(DIST_DIR)/qlwt 2>/dev/null || true
	@libdir=$$(pkg-config --variable=libdir glfw3 2>/dev/null || echo /usr/lib); \
	 copied=0; \
	 for f in $$libdir/libglfw.so.3 $$libdir/libglfw.so.3.*; do \
	     if [ -e "$$f" ]; then cp -P "$$f" $(DIST_DIR)/lib/; copied=1; fi; \
	 done; \
	 if [ "$$copied" = "1" ]; then echo "  随包带上 libglfw.so.3"; \
	 else echo "  ！没找到系统 libglfw.so.3：目标机器需要自行安装 glfw"; fi
	@cp -a assets $(DIST_DIR)/assets
	@cp -a docs $(DIST_DIR)/docs
	@cp README.md $(DIST_DIR)/README.md
	@tar -C dist -czf dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	@echo "发行版就绪：$(DIST_DIR)/qlwt"
	@du -sh $(DIST_DIR) dist/$(DIST_NAME).tar.gz

# Windows 版交叉编译（自带 GLFW 源码，详见 tools/build_windows.sh）
release-windows:
	@bash tools/build_windows.sh

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GFX_CFLAGS) -MMD -MP -c $< -o $@

run: $(TARGET)
	./$(TARGET)

check: $(TARGET)
	@for f in assets/scripts/*.gal; do \
	    case "$$(basename $$f)" in test*|_*) continue;; esac; \
	    echo "== $$f =="; \
	    ./$(TARGET) --check-script   $$f || exit 1; \
	    ./$(TARGET) --check-chapters $$f || exit 1; \
	    ./$(TARGET) --check-save     $$f || exit 1; \
	done
	@echo "== 剧情树 =="
	@./$(TARGET) --tree
	@echo "== 素材 =="
	@./$(TARGET) --check-assets
	@echo "== 观测战 =="
	@./$(TARGET) --check-battle
	@echo "== 触摸板手势 =="
	@./$(TARGET) --check-gesture

# 系统里的中文字体基本都是 .ttc，而引擎只认 .ttf/.otf，所以抽一份出来。
font:
	@mkdir -p assets/fonts
	python3 tools/ttc2ttf.py "$$(fc-match -f '%{file}' :lang=zh-cn)" \
	    assets/fonts/NotoSansSC-Regular.ttf SC

clean:
	rm -rf $(OBJDIR) $(TARGET)
	@echo "clean OK"

help:
	@sed -n '2,17p' Makefile

-include $(DEPS)
