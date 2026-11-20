#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 简中间 的输出渲染成一页能点的 HTML。

用法：
    ./简中间 示例/泛型.jy | python3 生成可视化.py -o 示例/泛型_中间表示.html
    python3 生成可视化.py 中间表示.txt -o 看.html

做的事情很朴素：按行分类，套上不同的颜色和标签。
目的只有一个——让"树倒了之后长什么样"这件事，一眼能看明白。
"""
import html
import re
import sys

# ---------------------------------------------------------------- 颜色分类
# 同一类指令一个颜色，读的时候眼睛能按颜色跳
分类 = [
    ("返回", ["返回"], "red"),
    ("跳转", ["跳 ", "条件跳"], "yellow"),
    ("调用", ["调用", "调用方法", "取函数"], "orange"),
    ("比较", ["等于", "不等", "小于", "大于"], "purple"),
    ("算术", ["加 ", "减 ", "乘 ", "除 ", "整除", "取余", "乘方",
              "位与", "位或", "位异或", "左移", "右移", "取反", "负 ", "非 ",
              "拼接", "长度", "转换"], "blue"),
    ("数据", ["常量", "载 ", "存 ", "存字段", "存下标", "取字段", "取属性", "取下标"], "cyan"),
    ("构造", ["构造组", "构造映射", "构造结构", "构造变体", "成功", "失败"], "green"),
    ("并发", ["发送", "接收", "建通道", "关通道", "等待", "遍历", "匹配变体", "解构变体"], "pink"),
]

颜色表 = {
    "red": "#ff7b72", "yellow": "#e3b341", "orange": "#ffa657",
    "purple": "#d2a8ff", "blue": "#79c0ff", "cyan": "#56d4dd",
    "green": "#7ee787", "pink": "#ff9bce", "plain": "#c9d1d9",
    "dim": "#6e7681",
}


def 指令颜色(文):
    for _, 关键词, 色 in 分类:
        for k in 关键词:
            if k in 文:
                return 色
    if 文.strip().startswith(";"):
        return "dim"
    return "plain"


# ---------------------------------------------------------------- 行渲染
值号 = re.compile(r"%t(\d+)")
变量 = re.compile(r"%([^\s,()\[\]:]+)")
标签 = re.compile(r"\bL(\d+)\b")


def 渲染行(行, 当前函数):
    if not 行.strip():
        return ""
    原 = html.escape(行)

    # 块标签：做成可点的锚点
    原 = 标签.sub(lambda m: f'<a class="lbl" href="#f{当前函数}-L{m.group(1)}">L{m.group(1)}</a>', 原)

    # 值号：临时值一种颜色，变量另一种
    原 = 值号.sub(lambda m: f'<span class="tid">%t{m.group(1)}</span>', 原)
    原 = 变量.sub(lambda m: f'<span class="var">%{m.group(1)}</span>', 原)

    色 = 指令颜色(行)
    return f'<div class="ins" data-k="{色}" style="--c:{颜色表[色]}">{原}</div>'


def 渲染函数(名, 行们, 序号):
    头 = 行们[0]
    体 = 行们[1:]

    块们 = []
    当前块 = None
    for 行 in 体:
        m = re.match(r"^(L\d+):(.*)$", 行)
        if m:
            当前块 = {"名": m.group(1), "注": m.group(2).strip(), "行": []}
            块们.append(当前块)
        elif 当前块 is not None:
            当前块["行"].append(行)

    if not 块们:  # 统计模式，只有一行 “...”
        块们 = [{"名": "", "注": "", "行": 体}]

    块html = []
    for b in 块们:
        锚 = f'f{序号}-{b["名"]}' if b["名"] else f'f{序号}'
        注 = f'<span class="note">{html.escape(b["注"])}</span>' if b["注"] else ""
        行html = "\n".join(渲染行(x, 序号) for x in b["行"])
        标签html = f'<div class="blk-tag" id="{锚}">{b["名"]}</div>' if b["名"] else ""
        块html.append(f'<div class="block">{标签html}{注}\n{行html}</div>')

    指令数 = sum(1 for x in 体 if x.strip() and not x.strip().startswith("L") and not x.strip().startswith("---"))

    return f'''
<section class="fn" id="f{序号}">
  <h2><code>{html.escape(头)}</code><span class="count">{指令数} 条</span></h2>
  {''.join(块html)}
</section>'''


def 主():
    参数 = sys.argv[1:]
    输出 = None
    if "-o" in 参数:
        i = 参数.index("-o")
        输出 = 参数[i + 1]
        del 参数[i:i + 2]

    文本 = open(参数[0], encoding="utf-8").read() if 参数 else sys.stdin.read()
    行们 = 文本.splitlines()

    标题 = "简言 · 中间表示"
    函数们 = []
    当前 = None
    for 行 in 行们:
        if 行.startswith("=== "):
            标题 = 行.strip("= ").strip()
            continue
        if re.match(r"^函数 .+\(.*\)", 行):
            if 当前:
                函数们.append(当前)
            当前 = [行]
        elif 当前 is not None:
            当前.append(行)
    if 当前:
        函数们.append(当前)

    # 统计块单独拿出来
    统计 = []
    for i, f in enumerate(函数们):
        位置 = next((j for j, x in enumerate(f) if x.startswith("--- 统计 ---")), None)
        if 位置 is not None:
            统计 = f[位置:]
            函数们[i] = f[:位置]
            break

    导航 = "".join(
        f'<a href="#f{i}">{html.escape(f[0][3:].split("(")[0])}</a>'
        for i, f in enumerate(函数们))

    正文 = "".join(渲染函数(f[0], f, i) for i, f in enumerate(函数们))

    统计html = ""
    if 统计:
        行html = "".join(f'<div class="stat-line">{html.escape(x)}</div>' for x in 统计 if x.strip())
        统计html = f'<section class="stats"><h2>统计</h2>{行html}</section>'

    页 = f'''<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(标题)}</title>
<style>
:root {{
  --bg:#0d1117; --panel:#161b22; --line:#30363d; --fg:#c9d1d9; --dim:#6e7681;
  --mono:"JetBrains Mono","SF Mono",Consolas,"Noto Sans Mono CJK SC",monospace;
}}
* {{ box-sizing:border-box; }}
body {{
  margin:0; background:var(--bg); color:var(--fg);
  font-family:-apple-system,"Segoe UI","Noto Sans CJK SC",sans-serif;
  display:grid; grid-template-columns:240px 1fr; min-height:100vh;
}}
aside {{
  position:sticky; top:0; height:100vh; overflow:auto;
  border-right:1px solid var(--line); padding:20px 14px; background:var(--panel);
}}
aside h1 {{ font-size:15px; margin:0 0 4px; font-weight:600; letter-spacing:.5px; }}
aside .sub {{ font-size:12px; color:var(--dim); margin-bottom:18px; line-height:1.6; }}
aside a {{
  display:block; padding:6px 10px; border-radius:6px; color:var(--fg);
  text-decoration:none; font-family:var(--mono); font-size:13px;
  white-space:nowrap; overflow:hidden; text-overflow:ellipsis;
}}
aside a:hover {{ background:#21262d; }}
main {{ padding:28px 36px 80px; max-width:1180px; }}
header h1 {{ font-size:22px; margin:0 0 6px; }}
header p {{ color:var(--dim); font-size:13px; margin:0 0 26px; }}
.fn {{ margin-bottom:34px; }}
.fn h2 {{
  font-size:14px; font-weight:500; margin:0 0 10px; color:#e6edf3;
  display:flex; align-items:baseline; gap:12px;
}}
.fn h2 code {{ font-family:var(--mono); font-size:13.5px; }}
.count {{ font-size:11px; color:var(--dim); font-weight:400; }}
.block {{
  background:var(--panel); border:1px solid var(--line); border-radius:8px;
  padding:24px 14px 12px 14px; margin-bottom:10px; position:relative;
  font-family:var(--mono); font-size:13px; line-height:1.85;
  overflow-x:auto;
}}
.blk-tag {{
  position:absolute; left:-1px; top:-1px; background:#21262d;
  border:1px solid var(--line); border-radius:8px 0 8px 0;
  padding:1px 9px; font-size:11px; color:#8b949e; font-weight:600;
  letter-spacing:.4px;
}}
.blk-tag + .note {{ margin-left:44px; }}
.note {{ font-size:11px; color:var(--dim); }}
.ins {{ white-space:pre; }}
.ins::before {{
  content:""; display:inline-block; width:3px; height:13px;
  background:var(--c); border-radius:2px; margin-right:9px; vertical-align:-1px;
  opacity:.95;
}}
.ins:hover {{ background:#1f242c; border-radius:4px; }}
.tid {{ color:#79c0ff; background:#1f6feb22; padding:0 3px; border-radius:3px; }}
.var {{ color:#ffa657; background:#ffa65711; padding:0 3px; border-radius:3px; }}
.lbl {{
  color:#e3b341; text-decoration:none; border-bottom:1px dashed #e3b34166;
  font-weight:600;
}}
.lbl:hover {{ background:#e3b34122; }}
.stats {{ margin-top:40px; border-top:1px solid var(--line); padding-top:18px; }}
.stats h2 {{ font-size:13px; color:var(--dim); font-weight:500; margin:0 0 8px; }}
.stat-line {{ font-family:var(--mono); font-size:13px; color:#8b949e; line-height:1.9; }}
@media (max-width:820px) {{
  body {{ grid-template-columns:1fr; }}
  aside {{ position:static; height:auto; border-right:0; border-bottom:1px solid var(--line); }}
  main {{ padding:20px 16px 60px; }}
}}
</style></head>
<body>
<aside>
  <h1>简言 · 中间表示</h1>
  <div class="sub">三地址码 + 基本块<br>{len(函数们)} 个函数</div>
  {导航}
</aside>
<main>
  <header>
    <h1>{html.escape(标题)}</h1>
    <p>左边竖条是品类：<span style="color:#79c0ff">算术</span> ·
       <span style="color:#ffa657">调用</span> ·
       <span style="color:#e3b341">跳转</span> ·
       <span style="color:#ff7b72">返回</span> ·
       <span style="color:#56d4dd">数据搬运</span> ·
       <span style="color:#7ee787">构造</span></p>
  </header>
  {正文}
  {统计html}
</main>
</body></html>'''

    if 输出:
        open(输出, "w", encoding="utf-8").write(页)
        print(f"写好了：{输出}", file=sys.stderr)
    else:
        sys.stdout.write(页)


if __name__ == "__main__":
    主()
