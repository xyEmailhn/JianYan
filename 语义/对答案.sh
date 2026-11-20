#!/bin/sh
# 两份实现各出一份报告，去掉行号列号后逐字节比对。
# 去掉位置是因为 Python 那棵语法树不带位置信息，报错位置由 C 版负责保证。
set -e
DIR=$(dirname "$0")
cd "$DIR"

./简语义 示例/错误.jy 2>/dev/null \
  | sed -E 's/\[(错误|提示)\] 第 [0-9]+ 行 第 [0-9]+ 列：/[\1] /' > /tmp/c语义.txt || true
python3 sema_参考实现.py 示例/错误.jy 2>/dev/null > /tmp/py语义.txt || true

if diff -u /tmp/c语义.txt /tmp/py语义.txt; then
    echo "两份实现输出完全一致"
else
    echo "两份实现不一致，上面是差异"
    exit 1
fi
