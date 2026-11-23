#!/bin/bash
# 手机版的测试。分三层：
#   1. 解释器对拍——用桌面 gcc 编出来的解释器，跑后端那套用例，逐字比期望；
#   2. 能力边界——手机版不该支持的东西，必须给出「手机版暂不支持」，
#      而不是崩掉或者静默算出个错数；
#   3. 执行链路自测——写临时文件、fork、读管道、崩溃隔离。
cd "$(dirname "$0")/.." || exit 1

echo "== 编译桌面解释器 =="
gcc -O2 -Wall -Wextra -std=gnu99 -o /tmp/简解释 手机/解释.c -lm 2>&1 | head -20
[ -x /tmp/简解释 ] || { echo "编译失败"; exit 1; }

# 手机版明确不接的六个用例。列在这里，是为了让「NOSUP」也是一个受测行为：
# 它们必须返回非零，并且说清楚是哪一块没接。
NOSUP="图片 声音 大数 并发 网络 链式"

echo
echo "== 1. 解释器对拍 =="
OK=0; BAD=0; SKIP=0
for f in 后端/测试/*.jy; do
    b=$(basename "$f" .jy)
    case " $NOSUP " in *" $b "*) SKIP=$((SKIP+1)); continue;; esac
    out=$(timeout 30 /tmp/简解释 "$f" 2>&1)
    if [ "$out" = "$(cat "后端/测试/期望/$b.txt")" ]; then
        OK=$((OK+1))
    else
        BAD=$((BAD+1))
        echo "  ✗ $b"
        diff <(printf '%s\n' "$out") "后端/测试/期望/$b.txt" | head -6
    fi
done
echo "  通过 $OK，失败 $BAD，跳过 $SKIP（手机版不接的）"

echo
echo "== 2. 能力边界 =="
LBAD=0
for b in $NOSUP; do
    out=$(timeout 30 /tmp/简解释 "后端/测试/$b.jy" 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        echo "  ✗ $b：本该不支持，却正常返回了"
        LBAD=$((LBAD+1))
    elif printf '%s' "$out" | grep -q "手机版暂不支持"; then
        echo "  ✓ $b：$(printf '%s' "$out" | grep -o '手机版暂不支持[^ ]*.*' | head -1)"
    else
        echo "  ✗ $b：退出码 $rc，但没说清楚为什么"
        LBAD=$((LBAD+1))
    fi
done

echo
echo "== 3. 执行链路自测 =="
gcc -O2 -o /tmp/手机自测 手机/自测.c -lm 2>/dev/null
if timeout 180 /tmp/手机自测 | tail -3; then :; else LBAD=$((LBAD+1)); fi

echo
if [ $BAD -eq 0 ] && [ $LBAD -eq 0 ]; then
    echo "全部通过"
else
    echo "有失败：对拍 $BAD，边界 $LBAD"
    exit 1
fi
