#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qresult <qid> [timeout_s]\n"
            "       qresult <qid> --json\n"
            "       qresult <qid> --histogram\n"
            "\n"
            "Options:\n"
            "  <qid>        任务ID\n"
            "  [timeout_s]  等待超时秒数（默认 30）\n"
            "  --json       JSON格式输出\n"
            "  --histogram  ASCII柱状图输出\n"
            "  -h           帮助\n");
}

int main(int argc, char *argv[])
{
    qos_result_t *result;
    int qid         = 0;
    int timeout_s   = 30;
    int opt_json    = 0;
    int opt_histo   = 0;
    int i, ret;

    if (argc < 2) { usage(); return 1; }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(); return 0;
        } else if (strcmp(argv[i], "--json") == 0) {
            opt_json = 1;
        } else if (strcmp(argv[i], "--histogram") == 0) {
            opt_histo = 1;
        } else if (argv[i][0] != '-') {
            if (qid == 0)
                qid = atoi(argv[i]);
            else
                timeout_s = atoi(argv[i]);
        }
    }

    if (qid <= 0) {
        fprintf(stderr, "qresult: invalid qid\n");
        usage();
        return 1;
    }

    result = calloc(1, sizeof(*result));
    if (!result) return 1;

    printf("fetching result for qid=%d (timeout=%ds)...\n", qid, timeout_s);

    ret = qos_result(qid, result, timeout_s);

    if (ret == QOS_ERR_KERN) {
        /* 内核执行失败，error_code/error_info 已填写 */
        fprintf(stderr, "error: qid=%d state=FAILED "
                "(code=%d: %s)\n",
                qid, result->error_code, result->error_info);
        free(result);
        return 1;
    }

    if (ret != QOS_OK) {
        fprintf(stderr, "qresult: failed: %s\n", qos_err_str(ret));
        free(result);
        return 1;
    }

    if (opt_json) {
        char *buf = malloc(16384);
        if (buf) {
            int n = qos_result_to_json(result, buf, 16384);
            if (n > 0) printf("%s", buf);
            else fprintf(stderr, "qresult: JSON serialization failed\n");
            free(buf);
        }
    } else if (opt_histo) {
        qos_result_print_histogram(result);
    } else {
        qos_result_print(result);
    }

    free(result);
    return 0;
}