#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成观测战敌人的贴图（按形象类别），输出到 assets/enemy/。

出的是"敌方立绘卡"：主体占满画面、带环境氛围，引擎会把它裁进敌人卡片里显示
（不需要抠图）。想自己抠透明底走"浮动精灵"那条路的话加 --cutout。

依赖（与 make_character.py 同一套，不用装进项目）：
    /home/hoshino/project/ai-imagegen/ComfyUI          出图服务（先启动）
    /home/hoshino/project/ai-imagegen/cutout/.venv     本脚本运行环境（PIL / rembg）

用法：
    /home/hoshino/project/ai-imagegen/cutout/.venv/bin/python \
        tools/imagegen/make_enemies.py [--only light,cloud] [--seed 1234] [--cutout]

输出：
    assets/enemy/<类别>.png          引擎自动取用（见 assets/enemy/README.md）
    gen_preview/enemy_<类别>_raw.png 出图原图，方便对照 / 重挑
"""

import argparse
import json
import os
import sys
import time
import urllib.request

from PIL import Image, ImageDraw, ImageOps

API = "http://127.0.0.1:8188"
COMFY = "/home/hoshino/project/ai-imagegen/ComfyUI"
OUT_DIR = os.path.join(COMFY, "output")
PROJ = "/home/hoshino/project/GalgameScriptEngine"
DEST = os.path.join(PROJ, "assets", "enemy")
PREVIEW = os.path.join(PROJ, "gen_preview")

MODEL = "animagine-xl-3.1.safetensors"
STEPS, CFG = 28, 5.5
MAX_SIDE = 512          # 输出贴图最长边（引擎会等比塞进 ~210x150 的卡片）
CARD_W, CARD_H = 560, 400   # 输出构图：敌人卡片约 210x150（1.4:1），留两倍余量

COMMON_POS = (", single subject, fills the frame, centered, anime style, cel shading, "
              "clean lineart, soft even lighting, clearly visible, mid tone background, "
              "muted colors, visual novel enemy portrait, game asset")
COMMON_NEG = ("lowres, worst quality, low quality, bad anatomy, text, watermark, signature, "
              "humans, people, 1girl, 1boy, multiple subjects, duplicate, cluttered, "
              "photo, 3d render, realistic, oversaturated, hdr, frame, border, ui, "
              "too dark, underexposed, near black image, black screen, heavy vignette")

# 类别 → 提示词（类别名就是输出文件名，也是引擎的查找键）
ENEMIES = {
    "light": dict(
        cn="光 / 灯 / 霓虹",
        pos=("huge glowing sphere of warm golden light, floating light particles, soft bloom, "
             "dusk sky with city lights blurred behind, warm glow lights up the scene"
             + COMMON_POS),
        neg="flat lighting"),
    "cloud": dict(
        cn="云 / 雨 / 雾",
        pos=("towering rain cloud, heavy downpour, bright grey overcast sky, wet air, "
             "soft volumetric edges, slightly menacing but clearly visible" + COMMON_POS),
        neg="sunny, bright blue sky, cheerful, pastel"),
    "noise": dict(
        cn="杂讯 / 噪音",
        pos=("swirling mass of magenta purple static noise, glitch pixels, digital signal "
             "interference, scanlines, glowing against a grey background" + COMMON_POS),
        neg="clean, smooth, calm, pastel"),
    "solid": dict(
        cn="其余（旧纪录 / 空白 / 停摆 …）",
        pos=("large floating hexagonal crystal shard, dark muted red, faint paper texture and "
             "ink lines, geometric, sharp edges, soft grey background" + COMMON_POS),
        neg="organic, fluffy, cute"),
}


def queue(workflow, client="enemy"):
    req = urllib.request.Request(
        API + "/prompt",
        data=json.dumps({"prompt": workflow, "client_id": client}).encode(),
        headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=30).read())["prompt_id"]


def wait_image(prompt_id, timeout=900):
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(2)
        h = json.loads(urllib.request.urlopen(f"{API}/history/{prompt_id}", timeout=30).read())
        if prompt_id in h:
            for node in h[prompt_id]["outputs"].values():
                for img in node.get("images", []):
                    p = os.path.join(OUT_DIR, img.get("subfolder", ""), img["filename"])
                    return p, time.time() - t0
    raise TimeoutError("ComfyUI 出图超时")


def txt2img(pos, neg, seed, prefix, size, steps):
    wf = {
        "4": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": MODEL}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": size, "height": size, "batch_size": 1}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": pos, "clip": ["4", 1]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["4", 1]}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": CFG,
                         "sampler_name": "euler_ancestral", "scheduler": "normal",
                         "denoise": 1.0, "model": ["4", 0], "positive": ["6", 0],
                         "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["4", 2]}},
        "9": {"class_type": "SaveImage",
              "inputs": {"filename_prefix": prefix, "images": ["8", 0]}},
    }
    return wait_image(queue(wf))


_SESSION = None


def cutout(rgba):
    """抠透明底：先试动漫分割模型，认不出主体就改用"白底反解"。

    敌人是发光球 / 云团这类抽象物体，动漫分割模型（isnet-anime 只认角色）
    经常整张判成背景，所以主用白底反解：亮度越高越透明，并按"白底合成"
    反解出原始颜色 —— 辉光会自然变成半透明，比硬阈值干净。
    """
    global _SESSION
    try:
        if _SESSION is None:
            from rembg import new_session
            _SESSION = new_session("isnet-anime")
        from rembg import remove
        out = remove(rgba, session=_SESSION, alpha_matting=False)
        if coverage(out) > 0.02:
            return out, "rembg"
    except Exception as e:                       # noqa: BLE001 - 缺模型/依赖都走兜底
        print(f"    ! 动漫分割不可用（{e}）")
    return key_white(rgba.convert("RGB")), "白底反解"


KEEP = 238.0     # 亮度 >= 这个值 = 纯背景，全透明
FULL = 196.0     # 亮度 <= 这个值 = 主体，不透明


def key_white(rgb):
    """白底 → 透明：alpha 由亮度推，颜色按 C = S*a + 255*(1-a) 反解。"""
    w, h = rgb.size
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    src, dst = rgb.load(), out.load()
    span = KEEP - FULL
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            lum = max(r, g, b)                   # 用最大通道，彩色主体不容易被误伤
            t = (KEEP - lum) / span
            if t <= 0.0:
                continue                          # 纯背景
            if t >= 1.0:
                dst[x, y] = (r, g, b, 255)
                continue
            # 反白：把被白底冲淡的颜色解出来，避免边缘一圈白边
            inv = 255.0 * (1.0 - t)
            nr = (r - inv) / t
            ng = (g - inv) / t
            nb = (b - inv) / t
            dst[x, y] = (0 if nr < 0 else (255 if nr > 255 else int(nr + 0.5)),
                         0 if ng < 0 else (255 if ng > 255 else int(ng + 0.5)),
                         0 if nb < 0 else (255 if nb > 255 else int(nb + 0.5)),
                         int(t * 255.0 + 0.5))
    return out


def crop_to_card(img):
    """居中裁成敌人卡片的比例，再缩到输出尺寸"""
    target = CARD_W / float(CARD_H)
    w, h = img.size
    if w / float(h) > target:                 # 太宽 → 裁两边
        nw = int(h * target)
        img = img.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    else:                                     # 太高 → 裁上下（略偏上，留地面）
        nh = int(w / target)
        top = int((h - nh) * 0.35)
        img = img.crop((0, top, w, top + nh))
    return img.resize((CARD_W, CARD_H), Image.LANCZOS)


def normalize_brightness(img, target=112.0):
    """拉一下对比度并把平均亮度归到 target。

    模型（animagine 是画角色的）对"云 / 噪点"这类物体常出很暗的图，
    直接放进深色战斗背景里就是一团黑，所以这里统一兜一道底。
    """
    import math
    rgb = ImageOps.autocontrast(img.convert("RGB"), cutoff=1)
    mean = sum(i * n for i, n in enumerate(rgb.convert("L").histogram())) / float(
        rgb.width * rgb.height)
    if mean < 1.0 or mean > 254.0:
        return rgb
    gamma = math.log(target / 255.0) / math.log(mean / 255.0)
    gamma = max(0.35, min(2.2, gamma))
    lut = [min(255, int(255.0 * ((v / 255.0) ** gamma) + 0.5)) for v in range(256)]
    return rgb.point(lut * 3)


def contact_sheet(paths, dst):
    """把几张卡片拼成一张预览图，方便一眼看效果"""
    cols, rows = 2, (len(paths) + 1) // 2
    cell_w, cell_h, pad, label = CARD_W, CARD_H + 26, 12, 22
    sheet = Image.new("RGB", (cols * cell_w + pad * 3, rows * cell_h + pad * 3),
                      (24, 26, 34))
    draw = ImageDraw.Draw(sheet)
    for i, (name, p) in enumerate(paths):
        cx = pad + (i % cols) * (cell_w + pad)
        cy = pad + (i // cols) * (cell_h + pad)
        sheet.paste(Image.open(p).convert("RGB"), (cx, cy))
        draw.text((cx + 6, cy + CARD_H + 5), name, fill=(220, 224, 232))
    sheet.save(dst)


def coverage(img):
    a = img.getchannel("A")
    n = sum(1 for v in a.getdata() if v > 24)
    return n / float(a.width * a.height)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只做这几个类别，逗号分隔")
    ap.add_argument("--seed", type=int, default=20261005)
    ap.add_argument("--steps", type=int, default=STEPS)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--cutout", action="store_true",
                    help="抠成透明底（默认不抠，直接当敌方立绘卡用）")
    args = ap.parse_args()

    want = [s for s in args.only.split(",") if s] or list(ENEMIES)
    os.makedirs(DEST, exist_ok=True)
    os.makedirs(PREVIEW, exist_ok=True)

    made = []
    for i, key in enumerate(want):
        if key not in ENEMIES:
            print(f"跳过未知类别 {key}")
            continue
        e = ENEMIES[key]
        seed = args.seed + i * 977
        print(f"== {key}（{e['cn']}）seed={seed} ==")
        raw_path, took = txt2img(e["pos"], e["neg"], seed, f"enemy_{key}", args.size,
                                 args.steps)
        raw = Image.open(raw_path).convert("RGBA")
        raw.save(os.path.join(PREVIEW, f"enemy_{key}_raw.png"))

        if args.cutout:
            cut, how = cutout(raw)
            out = crop_to_card(cut)
        else:
            out, how = crop_to_card(raw), "立绘卡"
        out = normalize_brightness(out)
        made.append((key, os.path.join(DEST, f"{key}.png")))
        dst = os.path.join(DEST, f"{key}.png")
        out.save(dst)
        # 亮度自检：太暗的图放进深色战斗背景里会像一团黑
        gray = out.convert("L")
        lum = sum(i * n for i, n in enumerate(gray.histogram())) / float(gray.width * gray.height)
        print(f"   {took:.0f}s  → {dst}  {out.width}x{out.height}  "
              f"平均亮度 {lum:.0f}/255（{how}）"
              + ("  ← 偏暗，建议换 seed 重出" if lum < 70 else ""))

    if len(made) > 1:
        sheet = os.path.join(PREVIEW, "enemy_cards.png")
        contact_sheet(made, sheet)
        print(f"预览拼图：{sheet}")
    print("完成。引擎会在战斗里自动取用 assets/enemy/<类别>.png")


if __name__ == "__main__":
    sys.exit(main())
