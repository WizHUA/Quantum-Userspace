#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qrun [options] <circuit.qasm>\n"
            "       qrun [options] -e <inline_qasm>\n"
            "\n"
            "Options:\n"
            "  -s <shots>     测量次数（默认1000）\n"
            "  -p <priority>  优先级 0~9（默认0）\n"
            "  -w             提交后等待结果\n"
            "  -h             显示帮助\n"
            "\n"
            "Examples:\n"
            "  qrun bell.qasm\n"
            "  qrun -s 2000 -w bell.qasm\n");
}

/* 从文件读取线路 */
static char *read_circuit_file(const char *path)
{
    FILE *f;
    long  sz;
    char *buf;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "qrun: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);

    buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, sz, f);
    if (n != (size_t)sz) {
        fprintf(stderr, "fread: short read\n");
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char *argv[])
{
    qos_config_t  cfg = { .shots = 1000, .priority = 0 };
    qos_result_t *result;
    const char   *circuit_str  = NULL;
    char         *circuit_file = NULL;
    int           wait_result  = 0;
    int           qid, i;

    /* 解析参数 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(); return 0;
        } else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) {
            cfg.shots = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            cfg.priority = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0) {
            wait_result = 1;
        } else if (strcmp(argv[i], "-e") == 0 && i+1 < argc) {
            circuit_str = argv[++i];
        } else {
            /* 最后一个非选项参数为文件路径 */
            circuit_file = argv[i];
        }
    }

    if (!circuit_str && !circuit_file) {
        usage(); return 1;
    }

    /* 读取线路内容 */
    if (circuit_file) {
        circuit_str = read_circuit_file(circuit_file);
        if (!circuit_str) return 1;
    }

    /* 提交任务 */
    qid = qos_exec(circuit_str, &cfg);
    if (circuit_file) free((void *)circuit_str);

    if (qid < 0) {
        fprintf(stderr, "qrun: submit failed (err=%d)\n", qid);
        return 1;
    }
    printf("submitted: qid=%d  shots=%d  priority=%d\n",
           qid, cfg.shots, cfg.priority);

    /* -w 等待并打印结果 */
    if (wait_result) {
        result = calloc(1, sizeof(*result));
        if (!result) return 1;

        printf("waiting for result...\n");
        if (qos_result(qid, result, 30) == QERR_OK)
            qos_result_print(result);
        else
            fprintf(stderr, "qrun: failed to get result\n");

        free(result);
    }

    return 0;
}