#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把语法分析器打印出来的树，变成一张能点开、能收起的网页。

用法：
    ../语法/简语法 源文件.简 > 树.txt
    python3 生成可视化.py 树.txt 源文件.简 语法树可视化.html

树.txt 的格式很简单：一行一个节点，缩进两格表示"我是上面那个的孩子"。
这个脚本干三件事：
    1. 按缩进把扁平的文本重新拼成树
    2. 按节点种类上色（声明一种色、语句一种色、表达式一种色）
    3. 生成一个自带 CSS/JS 的 HTML，双击就能看
"""

import html
import json
import os
import sys

# 节点种类 → (大类, 颜色)。颜色按"这一类东西在程序里扮演什么角色"分：
#   程序骨架  灰蓝   声明  紫   语句  橙   表达式  绿   字面量  青   其他  灰
分类 = {
    "程序": ("骨架", "#64748b"), "模块": ("骨架", "#64748b"),
    "引入": ("骨架", "#64748b"), "导出": ("骨架", "#64748b"),

    "函数": ("声明", "#7c3aed"), "参数": ("声明", "#8b5cf6"),
    "泛型": ("声明", "#8b5cf6"), "结构": ("声明", "#7c3aed"),
    "字段": ("声明", "#a78bfa"), "枚举": ("声明", "#7c3aed"),
    "变体": ("声明", "#a78bfa"), "接口": ("声明", "#7c3aed"),
    "实现": ("声明", "#7c3aed"), "常量": ("声明", "#7c3aed"),
    "变量": ("声明", "#7c3aed"), "令": ("声明", "#7c3aed"),
    "类型": ("声明", "#c4b5fd"), "测试": ("声明", "#7c3aed"),
    "编译期": ("声明", "#7c3aed"), "宏": ("声明", "#7c3aed"),
    "外部": ("声明", "#7c3aed"),

    "块": ("语句", "#ea580c"), "返回": ("语句", "#f97316"),
    "如果": ("语句", "#ea580c"), "否则如果": ("语句", "#f97316"),
    "否则": ("语句", "#f97316"), "当": ("语句", "#ea580c"),
    "重复": ("语句", "#ea580c"), "对": ("语句", "#ea580c"),
    "匹配": ("语句", "#ea580c"), "例": ("语句", "#f97316"),
    "默认": ("语句", "#f97316"), "打断": ("语句", "#fb923c"),
    "继续": ("语句", "#fb923c"), "循环": ("语句", "#ea580c"),
    "线程": ("语句", "#ea580c"), "发送": ("语句", "#f97316"),
    "关闭": ("语句", "#f97316"), "不安全": ("语句", "#dc2626"),
    "表达式语句": ("语句", "#f97316"), "模式": ("语句", "#fdba74"),

    "调用": ("表达式", "#059669"), "成员": ("表达式", "#10b981"),
    "下标": ("表达式", "#10b981"), "一元": ("表达式", "#34d399"),
    "二元": ("表达式", "#059669"), "赋值": ("表达式", "#059669"),
    "名字": ("表达式", "#0d9488"), "借用": ("表达式", "#14b8a6"),
    "可变借用": ("表达式", "#14b8a6"), "复制": ("表达式", "#14b8a6"),
    "移动": ("表达式", "#14b8a6"), "成功": ("表达式", "#14b8a6"),
    "失败": ("表达式", "#14b8a6"), "等待": ("表达式", "#14b8a6"),
    "接收": ("表达式", "#14b8a6"), "通道": ("表达式", "#14b8a6"),
    "匿名函数": ("表达式", "#059669"),

    "整数": ("字面量", "#0284c7"), "浮点": ("字面量", "#0284c7"),
    "字符串": ("字面量", "#0284c7"), "真": ("字面量", "#0284c7"),
    "假": ("字面量", "#0284c7"), "空": ("字面量", "#0284c7"),
    "数组字面量": ("字面量", "#0ea5e9"), "映射字面量": ("字面量", "#0ea5e9"),
    "结构字面量": ("字面量", "#0ea5e9"), "变体构造": ("字面量", "#0ea5e9"),

    "错误": ("其他", "#dc2626"),
}


def 读树(路径):
    """把"一行一个节点 + 缩进"的文本，还原成嵌套的字典树"""
    行们 = []
    with open(路径, encoding="utf-8") as f:
        for 原行 in f:
            行 = 原行.rstrip("\n")
            if not 行.strip():
                continue
            缩进 = 0
            while 缩进 < len(行) and 行[缩进] == " ":
                缩进 += 1
            深 = 缩进 // 2
            内容 = 行[缩进:]

            # 内容形如：种类 [文字] [标志]    标志在最后一个方括号里
            标志 = []
            if 内容.endswith("]") and " [" in 内容:
                位置 = 内容.rfind(" [")
                标志 = 内容[位置 + 2:-1].split()
                内容 = 内容[:位置]

            部分 = 内容.split(" ", 1)
            种类 = 部分[0]
            文字 = 部分[1] if len(部分) > 1 else ""
            行们.append((深, 种类, 文字, 标志))

    根 = None
    栈 = []
    for 深, 种类, 文字, 标志 in 行们:
        节 = {"种类": 种类, "文字": 文字, "标志": 标志, "子": []}
        while 栈 and 栈[-1][0] >= 深:
            栈.pop()
        if 栈:
            栈[-1][1]["子"].append(节)
        else:
            根 = 节
        栈.append((深, 节))
    return 根


def 统计(节, 计数, 深=0):
    计数[节["种类"]] = 计数.get(节["种类"], 0) + 1
    最深 = 深
    for 子 in 节["子"]:
        最深 = max(最深, 统计(子, 计数, 深 + 1))
    return 最深


def 造网页(根, 源路径, 源文本):
    计数 = {}
    最深 = 统计(根, 计数)
    总节点 = sum(计数.values())

    图例 = []
    for 大类, 色 in [("骨架", "#64748b"), ("声明", "#7c3aed"), ("语句", "#ea580c"),
                     ("表达式", "#059669"), ("字面量", "#0284c7"), ("其他", "#dc2626")]:
        图例.append('<span class="chip" style="--c:%s">%s</span>' % (色, 大类))

    分布 = sorted(计数.items(), key=lambda x: -x[1])
    分布条 = "".join(
        '<div class="row"><span class="k">%s</span>'
        '<span class="bar"><i style="width:%.1f%%;background:%s"></i></span>'
        '<span class="v">%d</span></div>'
        % (html.escape(名), 100.0 * 数 / max(1, 总节点),
           分类.get(名, ("其他", "#94a3b8"))[1], 数)
        for 名, 数 in 分布[:14]
    )

    源码行数 = 源文本.count("\n") + 1

    return """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>简言语语法树 · %(标题)s</title>
<style>
  :root {
    --bg: #0f172a; --panel: #111c33; --line: #1e293b;
    --text: #e2e8f0; --dim: #94a3b8; --accent: #38bdf8;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font-family: "PingFang SC", "Microsoft YaHei", system-ui, sans-serif;
  }
  header {
    padding: 28px 40px 20px; border-bottom: 1px solid var(--line);
    background: linear-gradient(180deg, #132038, #0f172a);
  }
  h1 { margin: 0 0 8px; font-size: 22px; letter-spacing: .5px; }
  .sub { color: var(--dim); font-size: 13px; }
  .stats { display: flex; gap: 28px; margin-top: 18px; flex-wrap: wrap; }
  .stat b { display: block; font-size: 22px; color: var(--accent); font-weight: 600; }
  .stat span { font-size: 12px; color: var(--dim); }
  main { display: grid; grid-template-columns: 1fr 320px; gap: 0; min-height: calc(100vh - 190px); }
  #tree { padding: 20px 30px 60px; overflow-x: auto; }
  aside { border-left: 1px solid var(--line); padding: 22px 22px 60px; background: var(--panel); }
  aside h2 { font-size: 13px; color: var(--dim); text-transform: uppercase; letter-spacing: 1px; margin: 0 0 12px; }
  .chip { display: inline-block; padding: 3px 10px; margin: 0 6px 6px 0; border-radius: 99px;
          font-size: 12px; color: var(--c); border: 1px solid var(--c); }
  .node { font-family: "JetBrains Mono", "Cascadia Code", Consolas, monospace; font-size: 13.5px; line-height: 1.9; }
  .node > .self { cursor: pointer; display: inline-flex; align-items: baseline; gap: 8px; }
  .node > .self:hover .name { text-decoration: underline; text-underline-offset: 3px; }
  .toggle { width: 14px; color: #475569; font-size: 11px; user-select: none; }
  .kind { font-weight: 600; }
  .txt { color: #cbd5e1; }
  .flag { font-size: 11px; color: #fbbf24; border: 1px solid #78350f; background: #42200633;
          padding: 1px 6px; border-radius: 4px; }
  .children { margin-left: 18px; border-left: 1px dashed #1e293b; padding-left: 10px; }
  .node.closed > .children { display: none; }
  .row { display: grid; grid-template-columns: 74px 1fr 34px; gap: 8px; align-items: center;
         font-size: 12px; margin-bottom: 6px; }
  .row .k { color: #cbd5e1; font-family: monospace; }
  .row .bar { background: #1e293b; height: 8px; border-radius: 4px; overflow: hidden; }
  .row .bar i { display: block; height: 100%%; border-radius: 4px; }
  .row .v { text-align: right; color: var(--dim); font-variant-numeric: tabular-nums; }
  .note { font-size: 12.5px; color: var(--dim); line-height: 1.8; margin: 18px 0 0; }
  .note b { color: #e2e8f0; }
  .toolbar { margin-bottom: 14px; display: flex; gap: 8px; flex-wrap: wrap; }
  button { background: #1e293b; color: #cbd5e1; border: 1px solid #334155; border-radius: 6px;
           padding: 5px 12px; font-size: 12px; cursor: pointer; }
  button:hover { border-color: var(--accent); color: var(--accent); }
  code { background: #1e293b; padding: 1px 5px; border-radius: 4px; font-size: 12px; }
</style>
</head>
<body>
<header>
  <h1>语法树 · %(标题)s</h1>
  <div class="sub">源文件 %(源行数)d 行 → 词法切成词 → 语法分析搭成这棵树。点节点可以收起／展开。</div>
  <div class="stats">
    <div class="stat"><b>%(总节点)d</b><span>节点</span></div>
    <div class="stat"><b>%(最深)d</b><span>层深</span></div>
    <div class="stat"><b>%(种类数)d</b><span>种节点</span></div>
    <div class="stat"><b>%(源行数)d</b><span>行源码</span></div>
  </div>
</header>
<main>
  <div id="tree">
    <div class="toolbar">
      <button onclick="全部(true)">全部展开</button>
      <button onclick="全部(false)">全部收起</button>
      <button onclick="只看('声明')">只看声明</button>
      <button onclick="只看('语句')">只看语句</button>
      <button onclick="重置()">重置</button>
    </div>
    <div id="root" class="node"></div>
  </div>
  <aside>
    <h2>颜色代表什么</h2>
    %(图例)s
    <h2 style="margin-top:24px">节点分布</h2>
    %(分布条)s
    <p class="note">
      这棵树是<b>从源码一步步搭出来的</b>：<br>
      1. 词法器把源码切成词；<br>
      2. 语法分析器按语法规则，把这些词组合成节点；<br>
      3. 叶子是最小的东西（名字、数字），往上越走越大（表达式 → 语句 → 函数 → 整个程序）。<br><br>
      机器读代码只能从左往右，看懂"谁先算"要靠树。<br>
      比如 <code>a + b * c</code>，树上 <code>*</code> 在 <code>+</code> 下面，所以先算乘法。
    </p>
    <p class="note">数据来源：<code>%(源路径)s</code></p>
  </aside>
</main>
<script>
const 树 = %(数据)s;
const 分类 = %(分类)s;

function 造(节, 深) {
  const 信息 = 分类[节.种类] || ["其他", "#94a3b8"];
  const 壳 = document.createElement("div");
  壳.className = "node";
  壳.dataset.种类 = 节.种类;
  壳.dataset.类 = 信息[0];

  const 行 = document.createElement("div");
  行.className = "self";
  行.innerHTML =
    (节.子.length ? '<span class="toggle">▾</span>' : '<span class="toggle">·</span>') +
    '<span class="kind" style="color:' + 信息[1] + '">' + 节.种类 + '</span>' +
    (节.文字 ? '<span class="txt">' + 节.文字.replace(/&/g, "&amp;").replace(/</g, "&lt;") + '</span>' : '') +
    (节.标志.length ? 节.标志.map(f => '<span class="flag">' + f + '</span>').join("") : '');

  行.onclick = () => {
    if (!节.子.length) return;
    壳.classList.toggle("closed");
    行.querySelector(".toggle").textContent = 壳.classList.contains("closed") ? "▸" : "▾";
  };
  壳.appendChild(行);

  if (节.子.length) {
    const 盒 = document.createElement("div");
    盒.className = "children";
    节.子.forEach(子 => 盒.appendChild(造(子, 深 + 1)));
    壳.appendChild(盒);
  }
  return 壳;
}

document.getElementById("root").appendChild(造(树, 0));

function 全部(展开) {
  document.querySelectorAll(".node").forEach(n => {
    if (!n.querySelector(".children")) return;
    n.classList.toggle("closed", !展开);
    const t = n.querySelector(":scope > .self > .toggle");
    if (t) t.textContent = 展开 ? "▾" : "▸";
  });
}

function 只看(类) {
  全部(true);
  document.querySelectorAll(".node").forEach(n => {
    if (n.dataset.类 === 类) return;
    // 自己不是这一类、但孩子里有：留着当"路"
    if (n.querySelector('.node[data-类="' + 类 + '"]')) return;
    n.style.display = "none";
  });
}

function 重置() {
  document.querySelectorAll(".node").forEach(n => { n.style.display = ""; });
  全部(true);
}
</script>
</body>
</html>
""" % {
        "标题": html.escape(os.path.basename(源路径)),
        "源行数": 源码行数,
        "总节点": 总节点,
        "最深": 最深 + 1,
        "种类数": len(计数),
        "图例": "".join(图例),
        "分布条": 分布条,
        "源路径": html.escape(源路径),
        "数据": json.dumps(根, ensure_ascii=False),
        "分类": json.dumps(分类, ensure_ascii=False),
    }


def 主():
    if len(sys.argv) < 4:
        print("用法：python3 生成可视化.py 树.txt 源文件.简 输出.html", file=sys.stderr)
        return 2
    树文件, 源路径, 输出 = sys.argv[1], sys.argv[2], sys.argv[3]
    根 = 读树(树文件)
    with open(源路径, encoding="utf-8") as f:
        源文本 = f.read()
    with open(输出, "w", encoding="utf-8") as f:
        f.write(造网页(根, 源路径, 源文本))
    计数 = {}
    最深 = 统计(根, 计数)
    print("已生成 %s：%d 个节点，%d 层深" % (输出, sum(计数.values()), 最深 + 1))
    return 0


if __name__ == "__main__":
    sys.exit(主())
