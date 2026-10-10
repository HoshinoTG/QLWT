# 敌人贴图

战斗里敌人默认是用代码画的图形（发光球 / 云团 / 噪点 / 六边形）。
想换成贴图，有两种方式，优先级从高到低：

1. **剧本里显式指定**（某一个敌人专用）：

   ```gal
   init_battle 云层的阻挠 "积云" 160 12 8 assets/enemy/jiyun.png
   ```

2. **按形象类别自动生效**（一次给整类用，不用改剧本）：
   把图片放到这个目录，文件名固定为下面四个之一：

   | 文件名 | 名字里带这些字就归这类 |
   | --- | --- |
   | `light.png` | 光、灯、霓虹 |
   | `cloud.png` | 云、雨、雾 |
   | `noise.png` | 杂讯、噪音、流量 |
   | `solid.png` | 其余（六边形那类） |

3. 两个都没有 → 回落到代码画的图形。

## 当前这套是怎么来的

仓库里这 4 张是用项目自带的 ComfyUI 管线本地生成的（不是第三方素材），
提示词与流程在 `tools/imagegen/make_enemies.py`：

```bash
/home/hoshino/project/ai-imagegen/cutout/.venv/bin/python \
    tools/imagegen/make_enemies.py --only cloud --seed 313977
```

出图后会统一做一次亮度归一（保证在深色战斗背景上读得清），并把预览拼到
`gen_preview/enemy_cards.png`。不满意就换 `--seed` 重出某一张。

要求：

- **建议带透明底的 PNG**，人物/物体居中，长边 512~1024 就够
- 引擎会等比缩放塞进敌人卡片（约 210×150 的设计尺寸），底部留出名字与血条的位置
- 受击时会在贴图上叠一层白色闪光
- 路径必须是 **ASCII**（引擎在 Windows 下读不了中文路径），所以别用中文文件名
