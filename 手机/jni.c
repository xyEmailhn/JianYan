/* ============================================================================
 *  jni.c —— 手机壳与解释器之间的桥
 *  ----------------------------------------------------------------------------
 *  为什么用 fork 而不是直接调：
 *    编译器是"信任自己人"的代码——它假设输入是善意的。手机上的输入来自
 *    用户，而且解释器里全是裸指针。直接在主进程里跑，一段奇怪的程序就能
 *    把 App 带走。fork 之后跑在子进程里，段错误只死子进程，App 还能把
 *    "程序崩了"显示出来。
 *
 *  输出走管道：子进程把 stdout 和 stderr 都接到管道写端，父进程一次读干。
 *  这样诊断信息和 print 的顺序天然保持源码顺序，不会两条流各走各的。
 * ========================================================================== */

#define 手机_内嵌 1
#ifndef 手机_无JNI
#include <jni.h>
#endif
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#include "解释.c"

static void 写全文(const char *路, const char *文, int 长)
{
    int fd = open(路, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    int 已 = 0;
    while (已 < 长) {
        ssize_t n = write(fd, 文 + 已, (size_t)(长 - 已));
        if (n <= 0) break;
        已 += (int)n;
    }
    close(fd);
}

/* 编译并执行一段代码，把全部输出攒成一个字符串（malloc，调用方 free）。
 * 抽成独立函数是为了能在桌面上自测——JNI 那层太薄，不值得为它起模拟器。 */
char *简言_执行(const char *代码, const char *目录)
{
    char 路[1024];
    snprintf(路, sizeof(路), "%s/main.jy", 目录 ? 目录 : ".");
    写全文(路, 代码, (int)strlen(代码));

    int 管[2];
    if (pipe(管) != 0) return strdup("管道建不起来\n");

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程 */
        close(管[0]);
        dup2(管[1], 1);
        dup2(管[1], 2);
        close(管[1]);
        int 码 = 解_跑文件(路);
        fflush(stdout);
        _exit(码);
    }
    close(管[1]);

    /* 父进程：边读边攒 */
    size_t 容 = 8192, 长 = 0;
    char *缓 = (char *)malloc(容);
    if (!缓) { close(管[0]); return strdup("内存不够\n"); }
    for (;;) {
        if (长 + 4096 + 1 > 容) {
            容 *= 2;
            char *新 = (char *)realloc(缓, 容);
            if (!新) break;
            缓 = 新;
        }
        ssize_t n = read(管[0], 缓 + 长, 容 - 长 - 1);
        if (n <= 0) break;
        长 += (size_t)n;
    }
    close(管[0]);

    int 状态 = 0;
    waitpid(pid, &状态, 0);
    缓[长] = 0;

    if (WIFSIGNALED(状态)) {
        char 尾[160];
        snprintf(尾, sizeof(尾), "\n[程序异常终止：信号 %d]\n", WTERMSIG(状态));
        size_t 尾长 = strlen(尾);
        if (长 + 尾长 + 1 > 容) { 容 = 长 + 尾长 + 1; 缓 = (char *)realloc(缓, 容); }
        if (缓) { memcpy(缓 + 长, 尾, 尾长 + 1); }
    }

    return 缓;
}

#ifndef 手机_无JNI
JNIEXPORT jstring JNICALL
Java_ai_jianyan_MainActivity_run(JNIEnv *env, jclass cls,
                                 jstring j代码, jstring j目录)
{
    (void)cls;
    const char *代码 = (*env)->GetStringUTFChars(env, j代码, NULL);
    const char *目录 = (*env)->GetStringUTFChars(env, j目录, NULL);
    if (!代码 || !目录) return (*env)->NewStringUTF(env, "拿不到输入\n");

    char *输出 = 简言_执行(代码, 目录);
    jstring 结果 = (*env)->NewStringUTF(env, 输出 ? 输出 : "");
    free(输出);
    (*env)->ReleaseStringUTFChars(env, j代码, 代码);
    (*env)->ReleaseStringUTFChars(env, j目录, 目录);
    return 结果;
}
#endif
