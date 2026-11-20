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
[ "$FAIL" -eq 0 ]
