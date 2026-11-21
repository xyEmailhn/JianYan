#!/bin/bash
# 基准：同一个 .jy，分别用 C 后端（gcc -O2）和原生后端编译，各跑 3 次取最好。
# bash 的变量名只能是 ASCII，所以这里用 t0/t1/best 这种。
cd "$(dirname "$0")/.." || exit 1
[ -x ./简编译 ] || { echo "先 make"; exit 1; }

bench() {  # bench 程序 -> 毫秒
    local t0 t1 best=999999999 cost
    for _ in 1 2 3; do
        t0=$(date +%s%N); "$1" >/dev/null; t1=$(date +%s%N)
        cost=$(( (t1 - t0) / 1000000 ))
        [ "$cost" -lt "$best" ] && best=$cost
    done
    echo "$best"
}

printf "%-14s %10s %10s %8s\n" 用例 C后端 原生 倍数
for f in 基准/*.jy; do
    n=$(basename "$f" .jy)
    ./简编译 "$f" -o "/tmp/bc_$n" 2>/dev/null || { echo "$n C 编译失败"; continue; }
    c=$(bench "/tmp/bc_$n")
    if ./简编译 "$f" -n -o "/tmp/bn_$n" 2>/dev/null; then
        v=$(bench "/tmp/bn_$n")
        printf "%-14s %8dms %8dms %7.2fx\n" "$n" "$c" "$v" "$(echo "scale=2; $v/$c" | bc)"
    else
        printf "%-14s %8dms %10s\n" "$n" "$c" "不支持"
    fi
done
