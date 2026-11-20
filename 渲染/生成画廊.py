#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 出图/ 里的 PNG 和 示例/ 里的场景源码，打成一个自包含的 HTML 画廊。

用法：
    python3 生成画廊.py             # 生成 渲染画廊.html
    python3 生成画廊.py -o 相册.html

为什么内嵌 base64：这个页面要能拷到任何地方打开，不能依赖相对路径，
也不能假设对方有网络。
"""

import base64
import html
import os
import sys

这里 = os.path.dirname(os.path.abspath(__file__))
出图目录 = os.path.join(这里, "出图")
示例目录 = os.path.join(这里, "示例")

作品 = [
    ("静物.png", "静物 · 产品图", "无缝背景、三层布光、三种材质。二十行场景文件。"),
    ("小岛.png", "小岛 · 程序化建模", "盒 + 球 + 圆柱拼出来的微缩场景，同一个文件能导出 GLB。"),
    ("材质.png", "材质 · 玻璃与金属", "四种材质同台：玻璃、铬、金、磨砂玻璃。"),
    ("几何.png", "几何 · 粉彩插画", "长焦压缩透视，低饱和配色，大面积柔光。"),
]

场景名 = {"静物.png": "静物.scene", "小岛.png": "小岛.scene",
          "材质.png": "材质.scene", "几何.png": "几何.scene"}


def 转base64(路径):
    with open(路径, "rb") as f:
        return base64.b64encode(f.read()).decode("ascii")


def 读文本(路径):
    try:
        with open(路径, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""


def 主():
    输出 = "渲染画廊.html"
    if "-o" in sys.argv:
        输出 = sys.argv[sys.argv.index("-o") + 1]

    卡片 = []
    for 文件, 标题, 说明 in 作品:
        图路径 = os.path.join(出图目录, 文件)
        if not os.path.exists(图路径):
            continue
        b64 = 转base64(图路径)
        大小 = os.path.getsize(图路径) / 1024
        源 = 读文本(os.path.join(示例目录, 场景名.get(文件, "")))
        卡片.append(f"""
      <figure class="card">
        <img src="data:image/png;base64,{b64}" alt="{html.escape(标题)}" loading="lazy">
        <figcaption>
          <h2>{html.escape(标题)}</h2>
          <p>{html.escape(说明)}</p>
          <details>
            <summary>场景源码 · {len(源.splitlines())} 行 · {大小:.0f} KB</summary>
            <pre><code>{html.escape(源)}</code></pre>
          </details>
        </figcaption>
      </figure>""")

    页面 = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>简渲 · 渲染与建模</title>
<style>
  :root {{
    --底: #0f1216;
    --面: #171b21;
    --线: #262c35;
    --文: #d8dee8;
    --弱: #8b95a5;
    --亮: #e8b96a;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; background: var(--底); color: var(--文);
    font: 15px/1.7 -apple-system, "PingFang SC", "Noto Sans CJK SC", "Microsoft YaHei", sans-serif;
    -webkit-font-smoothing: antialiased;
  }}
  header {{
    padding: 72px 32px 48px; max-width: 1180px; margin: 0 auto;
  }}
  header .小标 {{ color: var(--亮); letter-spacing: .22em; font-size: 11px; text-transform: uppercase; }}
  header h1 {{ font-size: 42px; font-weight: 600; margin: 14px 0 10px; letter-spacing: -.02em; }}
  header p {{ color: var(--弱); max-width: 620px; margin: 0; }}
  .参数 {{ display: flex; flex-wrap: wrap; gap: 26px; margin-top: 30px; font-size: 13px; color: var(--弱); }}
  .参数 b {{ color: var(--文); font-weight: 600; }}
  main {{ max-width: 1180px; margin: 0 auto; padding: 0 32px 100px; display: grid; gap: 34px; }}
  .card {{
    margin: 0; background: var(--面); border: 1px solid var(--线);
    border-radius: 14px; overflow: hidden;
  }}
  .card img {{ display: block; width: 100%; height: auto; background: #000; }}
  figcaption {{ padding: 20px 24px 22px; }}
  figcaption h2 {{ font-size: 19px; margin: 0 0 6px; font-weight: 600; }}
  figcaption p {{ margin: 0; color: var(--弱); font-size: 14px; }}
  details {{ margin-top: 14px; border-top: 1px solid var(--线); padding-top: 12px; }}
  summary {{ cursor: pointer; color: var(--亮); font-size: 13px; list-style: none; }}
  summary::-webkit-details-marker {{ display: none; }}
  summary::before {{ content: "▸ "; }}
  details[open] summary::before {{ content: "▾ "; }}
  pre {{
    margin: 12px 0 0; padding: 16px; background: #0c0f13; border-radius: 8px;
    overflow-x: auto; font-size: 12.5px; line-height: 1.65;
  }}
  code {{ font-family: "JetBrains Mono", "SF Mono", Menlo, Consolas, monospace; color: #b8c4d4; }}
  footer {{ max-width: 1180px; margin: 0 auto; padding: 0 32px 80px; color: var(--弱); font-size: 13px; }}
  footer code {{ color: var(--文); }}
</style>
</head>
<body>
<header>
  <div class="小标">简言 · 标准库 v0.4</div>
  <h1>简渲</h1>
  <p>用中文写二十行场景，出一张图；同一个文件，再出一份 3D 模型。
     没有显卡、没有建模软件、没有第三方库——一个 108 KB 的二进制。</p>
  <div class="参数">
    <span><b>900 × 560</b> 分辨率</span>
    <span><b>128</b> 采样/像素</span>
    <span><b>4</b> 线程</span>
    <span><b>0.4 秒</b> 导出 15.5 万面</span>
  </div>
</header>
<main>{''.join(卡片)}
</main>
<footer>
  源码：<code>渲染/简渲.c</code>（约 2000 行 C99）· 场景：<code>渲染/示例/*.scene</code><br>
  渲染：距离场球追踪 + 面光源软阴影 + 环境光遮蔽 + 反射/折射 + ACES + 泛光<br>
  建模：marching tetrahedra → <code>.obj</code> / <code>.glb</code>
</footer>
</body>
</html>
"""
    with open(os.path.join(这里, 输出), "w", encoding="utf-8") as f:
        f.write(页面)
    总 = os.path.getsize(os.path.join(这里, 输出)) / 1024 / 1024
    print(f"写出 {输出}（{总:.1f} MB，{len(卡片)} 张图）")


if __name__ == "__main__":
    主()
