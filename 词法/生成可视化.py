#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 `简词法 -机器` 的输出画成一张彩色网页。

为什么要这个？
    词法分析器的输出是一堆"行:列 种类 文字"，对没写过编译器的人来说是抽象的。
    把每个词涂上颜色，一眼就能看出"哦，机器是按这个粒度理解代码的"。

用法：
    ./简词法 -机器 示例/词频.简 > tokens.txt
    python3 生成可视化.py tokens.txt 示例/词频.简 词法可视化.html
"""

import html
import sys
from collections import Counter

种类名 = ["文件结束", "换行", "标识符", "关键字", "整数", "浮点", "字符串", "符号", "错误"]
种类键 = ["eof", "nl", "ident", "kw", "int", "float", "str", "punct", "err"]


def 读词表(路径):
    词们 = []
    with open(路径, encoding="utf-8") as f:
        for 行 in f:
            行 = 行.rstrip("\n")
            if not 行:
                continue
            行号, 列, 种类, 起, 长 = 行.split("\t")
            词们.append({
                "行": int(行号), "列": int(列), "种类": int(种类),
                "起": int(起), "长": int(长),
            })
    return 词们


def 造网页(词们, 源字节, 标题):
    计数 = Counter(t["种类"] for t in 词们 if t["种类"] not in (0, 1))
    合计 = sum(计数.values())

    # ---- 正文：按字节偏移把源码切段，每段包一个带颜色的 span ----
    片段 = []
    上一个末尾 = 0
    for i, t in enumerate(词们):
        if t["种类"] == 0:
            break
        # 词与词之间被跳过的空白（空格、缩进），原样保留
        if t["起"] > 上一个末尾:
            空白 = 源字节[上一个末尾:t["起"]].decode("utf-8", "replace")
            片段.append(html.escape(空白))

        if t["种类"] == 1:                      # 换行：直接换行，不画框
            片段.append("\n")
            上一个末尾 = t["起"] + t["长"]
            continue

        文字 = 源字节[t["起"]:t["起"] + t["长"]].decode("utf-8", "replace")
        类 = 种类键[t["种类"]]
        提示 = f'第{t["行"]}行 第{t["列"]}列 · {种类名[t["种类"]]}'
        片段.append(
            f'<span class="t {类}" data-k="{类}" title="{html.escape(提示)}">{html.escape(文字)}</span>'
        )
        上一个末尾 = t["起"] + t["长"]

    正文 = "".join(片段)

    # ---- 图例 ----
    图例 = "".join(
        f'<button class="chip {种类键[i]}" data-k="{种类键[i]}" onclick="filter(this)">'
        f'<b>{种类名[i]}</b><i>{计数.get(i, 0)}</i></button>'
        for i in range(2, 9)
    )

    统计卡 = f"""
      <div class="stat"><b>{合计}</b><span>个词</span></div>
      <div class="stat"><b>{计数.get(2,0)}</b><span>个名字</span></div>
      <div class="stat"><b>{计数.get(3,0)}</b><span>个关键字</span></div>
      <div class="stat"><b>{len(源字节)}</b><span>字节源码</span></div>
    """

    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(标题)}</title>
<style>
  :root {{
    --bg: #14161a; --panel: #1b1e24; --line: #2a2f38; --fg: #d7dce4; --dim: #7b8494;
    --kw: #c792ea; --ident: #82aaff; --num: #f78c6c; --str: #c3e88d;
    --punct: #8b95a7; --err: #ff5370;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; background: var(--bg); color: var(--fg);
    font-family: "PingFang SC", "Microsoft YaHei", system-ui, sans-serif;
    line-height: 1.7; padding: 40px 24px 80px;
  }}
  .wrap {{ max-width: 1080px; margin: 0 auto; }}
  h1 {{ font-size: 24px; font-weight: 600; margin: 0 0 8px; letter-spacing: .5px; }}
  .sub {{ color: var(--dim); font-size: 14px; margin-bottom: 28px; }}
  .cards {{ display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 20px; }}
  .stat {{
    background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
    padding: 12px 18px; min-width: 108px;
  }}
  .stat b {{ display: block; font-size: 22px; font-weight: 600; font-variant-numeric: tabular-nums; }}
  .stat span {{ color: var(--dim); font-size: 12px; }}
  .legend {{ display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 18px; }}
  .chip {{
    display: inline-flex; align-items: center; gap: 6px; cursor: pointer;
    background: var(--panel); border: 1px solid var(--line); border-radius: 999px;
    padding: 5px 12px; font-size: 13px; color: var(--fg); font-family: inherit;
    transition: .15s;
  }}
  .chip i {{ font-style: normal; color: var(--dim); font-variant-numeric: tabular-nums; }}
  .chip:hover {{ border-color: var(--fg); }}
  .chip.on {{ border-color: currentColor; box-shadow: 0 0 0 1px currentColor inset; }}
  pre {{
    background: var(--panel); border: 1px solid var(--line); border-radius: 12px;
    padding: 22px 24px; overflow-x: auto; margin: 0 0 26px;
    font-family: "JetBrains Mono", "Cascadia Code", Menlo, Consolas, monospace;
    font-size: 14px; line-height: 1.85; tab-size: 4;
  }}
  .t {{ border-radius: 3px; padding: 1px 0; transition: .12s; }}
  .t:hover {{ background: #2d3440; outline: 1px solid #3d4654; }}
  .kw    {{ color: var(--kw);    font-weight: 600; }}
  .ident {{ color: var(--ident); }}
  .int, .float {{ color: var(--num); }}
  .str   {{ color: var(--str); }}
  .punct {{ color: var(--punct); }}
  .err   {{ color: var(--err); text-decoration: wavy underline; }}
  body.focus .t {{ opacity: .18; }}
  body.focus .t.match {{ opacity: 1; }}
  .foot {{ color: var(--dim); font-size: 13px; border-top: 1px solid var(--line); padding-top: 16px; }}
  .foot code {{ color: var(--fg); }}
</style>
</head>
<body>
<div class="wrap">
  <h1>简言源码，机器是这样读的</h1>
  <div class="sub">鼠标悬停任意一个有颜色的词，可以看到它在第几行第几列、被归为哪一类。点下面的色块可以只看某一类。</div>

  <div class="cards">{统计卡}</div>
  <div class="legend">{图例}</div>

  <pre>{正文}</pre>

  <div class="foot">
    颜色含义：<span class="kw">关键字</span> ·
    <span class="ident">名字</span> ·
    <span class="int">数字</span> ·
    <span class="str">文字</span> ·
    <span class="punct">符号</span>。
    空格、缩进、注释 <code>--</code> 都被丢弃了，不会出现在任何色块里。
  </div>
</div>
<script>
let 当前 = null;
function filter(btn) {{
  const k = btn.dataset.k;
  const body = document.body;
  if (当前 === k) {{
    当前 = null; body.classList.remove('focus');
    document.querySelectorAll('.chip').forEach(c => c.classList.remove('on'));
    document.querySelectorAll('.t').forEach(t => t.classList.remove('match'));
    return;
  }}
  当前 = k;
  body.classList.add('focus');
  document.querySelectorAll('.chip').forEach(c => c.classList.toggle('on', c.dataset.k === k));
  document.querySelectorAll('.t').forEach(t => t.classList.toggle('match', t.dataset.k === k));
}}
</script>
</body>
</html>
"""


def 主函数():
    if len(sys.argv) < 4:
        print("用法：python3 生成可视化.py 词表.txt 源码.简 输出.html", file=sys.stderr)
        return 2
    词们 = 读词表(sys.argv[1])
    with open(sys.argv[2], "rb") as f:
        源 = f.read()
    网页 = 造网页(词们, 源, "简言词法可视化")
    with open(sys.argv[3], "w", encoding="utf-8") as f:
        f.write(网页)
    print(f"已生成 {sys.argv[3]}（{len(词们)} 个词）")
    return 0


if __name__ == "__main__":
    sys.exit(主函数())
