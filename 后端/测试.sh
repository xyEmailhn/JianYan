#!/bin/bash
# 简言后端测试：编译每个 .jy，运行，和 期望/<名字>.txt 逐字比对。
# 只收确定性的示例：不依赖时间、随机数、外部文件。

cd "$(dirname "$0")" || exit 1
[ -x ./简编译 ] || { echo "先 make"; exit 1; }

PASS=0
FAIL=0

for f in 测试/*.jy; do
    [ -f "$f" ] || continue
    NAME=$(basename "$f" .jy)
    TMP=/tmp/jy_test_$$
    if ! ./简编译 "$f" -o "$TMP" >/dev/null 2>&1; then
        echo "✗ $NAME —— 编译失败"
        FAIL=$((FAIL + 1))
        continue
    fi
    ACTUAL=$(timeout 20 "$TMP" 2>&1)
    rm -f "$TMP"
    WANT="测试/期望/$NAME.txt"
    if [ ! -f "$WANT" ]; then
        echo "? $NAME —— 没有期望文件，跳过"
        continue
    fi
    if [ "$ACTUAL" = "$(cat "$WANT")" ]; then
        echo "✓ $NAME"
        PASS=$((PASS + 1))
    else
        echo "✗ $NAME —— 输出不符"
        diff <(echo "$ACTUAL") "$WANT" | head -20
        FAIL=$((FAIL + 1))
    fi
done

echo
echo "通过 $PASS，失败 $FAIL"

# ---------------------------------------------------------------- 原生后端对拍 --
# 原生后端（IR → 汇编 → as → ld）目前只覆盖 整数/布尔/字符串/控制流/调用。
# 所以这里不是「必须全过」，而是「编得出来就必须跑对」：
#   编不出来 → 记一个 ·，不算失败；
#   编得出来 → 输出必须和期望文件逐字节一致，错一个字就是失败。
# 这条规矩很重要：能编但编错，比编不出来危险得多。
NPASS=0
NFAIL=0
NSKIP=0
echo
echo "=== 原生后端对拍 ==="
for f in 测试/*.jy; do
    [ -f "$f" ] || continue
    NAME=$(basename "$f" .jy)
    WANT="测试/期望/$NAME.txt"
    [ -f "$WANT" ] || continue
    TMPN=/tmp/jy_native_$$
    if ! ./简编译 "$f" -n -o "$TMPN" >/dev/null 2>&1; then
        echo "· $NAME —— 原生后端还不支持，跳过"
        NSKIP=$((NSKIP + 1))
        continue
    fi
    ACTN=$(timeout 20 "$TMPN" 2>&1)
    rm -f "$TMPN"
    if [ "$ACTN" = "$(cat "$WANT")" ]; then
        echo "✓ 原生 $NAME"
        NPASS=$((NPASS + 1))
    else
        echo "✗ 原生 $NAME —— 与 C 后端不一致"
        diff <(echo "$ACTN") "$WANT" | head -20
        NFAIL=$((NFAIL + 1))
    fi
done
echo "原生：过 $NPASS，错 $NFAIL，跳过 $NSKIP"

[ "$FAIL" -eq 0 ] && [ "$NFAIL" -eq 0 ]
