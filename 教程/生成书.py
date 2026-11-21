#!/usr/bin/env python3
"""把 教程/*.md 拼成一本单文件 HTML 书：教程/教程.html。

为什么不用现成的静态站生成器：这个仓库的风格是"一个文件能双击打开"。
生成的 HTML 不依赖网络、不依赖 JS 框架，拷到哪儿都能读。

用法：
    python3 教程/生成书.py
"""
import html
import io
import os
import re

import markdown

HERE = os.path.dirname(os.path.abspath(__file__))

# 章的顺序与侧栏标题。加一章就在这里加一行。
CHAPTERS = [
    ("README.md", "封面 · 这本教程怎么读", "封面"),
    ("00-开始.md", "00 · 开始", "开始"),
    ("01-第一个程序.md", "01 · 第一个程序", "第一个程序"),
    ("02-变量与类型.md", "02 · 变量与类型", "变量与类型"),
    ("03-数字与运算符.md", "03 · 数字与运算符", "数字与运算符"),
    ("04-字符串.md", "04 · 字符串", "字符串"),
    ("05-条件与循环.md", "05 · 条件与循环", "条件与循环"),
    ("06-函数.md", "06 · 函数", "函数"),
    ("07-组与映射.md", "07 · 组与映射", "组与映射"),
    ("08-结构与方法.md", "08 · 结构与方法", "结构与方法"),
    ("09-枚举与匹配.md", "09 · 枚举与匹配", "枚举与匹配"),
    ("10-泛型.md", "10 · 泛型", "泛型"),
    ("11-可空与结果.md", "11 · 可空与结果", "可空与结果"),
    ("12-大整数与小数.md", "12 · 大整数与小数", "大整数与小数"),
    ("13-图与声音.md", "13 · 图与声音", "图与声音"),
    ("14-编译与性能.md", "14 · 编译与性能", "编译与性能"),
    ("15-报错速查.md", "15 · 报错速查", "报错速查"),
]

CSS = """
:root {
  --paper: #fbfaf7;
  --ink: #23282f;
  --dim: #6b7280;
  --line: #e5e2db;
  --code-bg: #f3f1ec;
  --accent: #2f6f4f;
  --accent-soft: #eaf3ee;
  --warn: #a2521a;
  --warn-bg: #fdf4ec;
}
* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
body {
  margin: 0;
  background: var(--paper);
  color: var(--ink);
  font: 16px/1.85 "Songti SC", "Noto Serif CJK SC", Georgia, serif;
  -webkit-font-smoothing: antialiased;
}
.shell { display: grid; grid-template-columns: 240px minmax(0, 760px); gap: 56px;
         max-width: 1120px; margin: 0 auto; padding: 0 28px 120px; }
@media (max-width: 900px) {
  .shell { grid-template-columns: 1fr; gap: 0; }
  nav { position: static !important; max-height: none !important; border: 0 !important;
        padding: 24px 0 0 !important; }
}
nav {
  position: sticky; top: 0; align-self: start;
  max-height: 100vh; overflow-y: auto;
  padding: 56px 0 40px;
  font-family: -apple-system, "PingFang SC", "Noto Sans CJK SC", sans-serif;
  font-size: 13px; line-height: 1.5;
  border-right: 1px solid var(--line);
}
nav .brand { font-size: 15px; font-weight: 600; letter-spacing: .5px; margin-bottom: 4px; }
nav .sub { color: var(--dim); font-size: 12px; margin-bottom: 22px; }
nav a { display: block; padding: 5px 12px 5px 10px; color: #4b5563;
        text-decoration: none; border-left: 2px solid transparent; }
nav a:hover { color: var(--accent); background: var(--accent-soft); }
nav a.on { color: var(--accent); border-left-color: var(--accent); font-weight: 600; }

main { min-width: 0; padding-top: 56px; }
section { padding-bottom: 72px; border-bottom: 1px solid var(--line); margin-bottom: 56px; }
section:last-child { border-bottom: 0; }

h1 { font-size: 30px; line-height: 1.4; margin: 0 0 22px; font-weight: 700; letter-spacing: -.3px; }
h2 { font-size: 21px; margin: 44px 0 12px; font-weight: 700; }
h3 { font-size: 17px; margin: 30px 0 8px; font-weight: 700; }
p { margin: 14px 0; }
a { color: var(--accent); }
strong { font-weight: 700; }
hr { border: 0; border-top: 1px solid var(--line); margin: 36px 0; }

blockquote {
  margin: 22px 0; padding: 12px 18px; border-left: 3px solid var(--accent);
  background: var(--accent-soft); color: #374151;
  font-size: 15.5px;
}
blockquote p { margin: 4px 0; }

code {
  font-family: "SF Mono", Consolas, "Noto Sans Mono CJK SC", monospace;
  font-size: .88em; background: var(--code-bg); padding: 1px 5px; border-radius: 3px;
}
pre {
  background: var(--code-bg); border: 1px solid var(--line); border-radius: 8px;
  padding: 16px 18px; overflow-x: auto; margin: 18px 0;
  font-family: "SF Mono", Consolas, "Noto Sans Mono CJK SC", monospace;
  font-size: 13px; line-height: 1.7;
}
pre code { background: none; padding: 0; font-size: inherit; }

table { border-collapse: collapse; width: 100%; margin: 20px 0; font-size: 14.5px;
        font-family: -apple-system, "PingFang SC", "Noto Sans CJK SC", sans-serif; }
th, td { border-bottom: 1px solid var(--line); padding: 8px 10px; text-align: left; vertical-align: top; }
th { font-weight: 600; color: #374151; border-bottom-width: 2px; }
tbody tr:hover { background: #f6f4ef; }

ul, ol { padding-left: 24px; margin: 14px 0; }
li { margin: 5px 0; }

.cover { border-bottom: 0; }
.cover .tag { font-family: -apple-system, "PingFang SC", sans-serif; font-size: 12px;
              color: var(--accent); letter-spacing: 3px; margin-bottom: 14px; }
.cover h1.book-title { font-size: 46px; line-height: 1.25; letter-spacing: -1.2px; margin: 0 0 14px; }
.cover .book-lead { font-size: 17px; color: var(--dim); margin: 0 0 34px; }
""".strip()

JS = """
(function () {
  var links = Array.prototype.slice.call(document.querySelectorAll('nav a'));
  var secs = links.map(function (a) { return document.getElementById(a.dataset.id); });
  function tick() {
    var y = window.scrollY + 120, best = 0;
    for (var i = 0; i < secs.length; i++) if (secs[i] && secs[i].offsetTop <= y) best = i;
    links.forEach(function (a, i) { a.classList.toggle('on', i === best); });
  }
  window.addEventListener('scroll', tick, { passive: true });
  window.addEventListener('resize', tick);
  tick();
})();
""".strip()


def convert(path: str) -> str:
    src = io.open(path, encoding="utf-8").read()
    # 去掉第一个 h1，标题由外层统一给（避免和侧栏重复两个大标题）
    src = re.sub(r"^#\s+.*?\n", "", src, count=1)
    return markdown.markdown(
        src, extensions=["tables", "fenced_code", "sane_lists"]
    )


def main() -> None:
    parts = []
    nav = []
    for i, (fn, title, short) in enumerate(CHAPTERS):
        path = os.path.join(HERE, fn)
        if not os.path.exists(path):
            continue
        sec_id = "ch%02d" % i
        body = convert(path)
        cls = "cover" if fn == "README.md" else ""
        if fn == "README.md":
            heading = ('<div class="tag">简言 · 教程</div>'
                       '<h1 class="book-title">从零开始学简言</h1>'
                       '<p class="book-lead">一门中文关键字的编译型语言。'
                       '十六个章节，每章配一个跑得起来的例子。</p>')
        else:
            heading = "<h1>%s</h1>" % html.escape(title)
        parts.append('<section id="%s" class="%s">%s\n%s</section>' % (sec_id, cls, heading, body))
        nav.append('<a href="#%s" data-id="%s">%s</a>' % (sec_id, sec_id, html.escape(short)))

    doc = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>简言教程</title>
<style>
%s
</style>
</head>
<body>
<div class="shell">
<nav>
  <div class="brand">简言教程</div>
  <div class="sub">从零到能写程序 · 每章都有可跑的例子</div>
  %s
</nav>
<main>
%s
</main>
</div>
<script>
%s
</script>
</body>
</html>
""" % (CSS, "\n  ".join(nav), "\n".join(parts), JS)

    out = os.path.join(HERE, "教程.html")
    io.open(out, "w", encoding="utf-8").write(doc)
    print("写到 %s（%.1f KB）" % (out, len(doc.encode("utf-8")) / 1024))


if __name__ == "__main__":
    main()
