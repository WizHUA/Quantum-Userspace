#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qresult <qid> [timeout_s]\n"
            "\n"
            "  <qid>       任务ID\n"
            "  [timeout_s] 等待超时秒数（默认30秒）\n");
}

int main(int argc, char *argv[])
{
    qos_result_t *result;
    int qid, timeout, ret;

    if (argc < 2) { usage(); return 1; }

    qid     = atoi(argv[1]);
    timeout = argc >= 3 ? atoi(argv[2]) : 30;

    result = calloc(1, sizeof(*result));
    if (!result) return 1;

    printf("fetching result for qid=%d (timeout=%ds)...\n",
           qid, timeout);

    ret = qos_result(qid, result, timeout);
    if (ret == QERR_OK) {
        qos_result_print(result);
    } else {
        fprintf(stderr, "qresult: failed (err=%d)\n", ret);
        free(result);
        return 1;
    }

    free(result);
    return 0;
}