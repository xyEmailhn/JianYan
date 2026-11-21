#!/bin/bash
# 教程自检：把 例子/*.jy 全部编一遍、跑一遍，和 期望/*.txt 逐字节比对。
# 任何一章的输出变了，这里就会红。加一章就加一对 .jy/.txt，不用改脚本。
#
# 注意：bash 的变量名只能是 ASCII，所以这里用 pass/fail/name 这种。
cd "$(dirname "$0")/.." || exit 1
[ -x ./后端/简编译 ] || { echo "先 cd 后端 && make"; exit 1; }

pass=0; fail=0
for f in 教程/例子/*.jy; do
    name=$(basename "$f" .jy)
    if ! ./后端/简编译 "$f" -o "/tmp/教程_$name" 2>"/tmp/教程_$name.err"; then
        echo "✗ $name 编译失败"
        head -3 "/tmp/教程_$name.err" | sed 's/^/    /'
        fail=$((fail + 1)); continue
    fi
    "/tmp/教程_$name" > "/tmp/教程_$name.out" 2>&1
    if diff -q "/tmp/教程_$name.out" "教程/期望/$name.txt" >/dev/null 2>&1; then
        echo "✓ $name"
        pass=$((pass + 1))
    else
        echo "✗ $name 输出不一致"
        diff "教程/期望/$name.txt" "/tmp/教程_$name.out" | head -6 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
done

echo
echo "教程例子：过 $pass，错 $fail"
[ "$fail" -eq 0 ]
