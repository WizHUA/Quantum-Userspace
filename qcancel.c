#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qcancel <qid>\n"
            "\n"
            "  取消一个处于 QUEUED 状态的任务\n"
            "  RUNNING 状态的任务无法取消\n"
            "\n"
            "Options:\n"
            "  -h   帮助\n");
}

int main(int argc, char *argv[])
{
    int qid, ret;

    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "-h") == 0) { usage(); return 0; }

    qid = atoi(argv[1]);
    if (qid <= 0) {
        fprintf(stderr, "qcancel: invalid qid '%s'\n", argv[1]);
        return 1;
    }

    ret = qos_cancel(qid);

    switch (ret) {
    case QOS_OK:
        printf("cancelled: qid=%d\n", qid);
        return 0;

    case QOS_ERR_NOT_FOUND:
        fprintf(stderr, "error: qid=%d not found\n", qid);
        return 1;

    case QOS_ERR_IOCTL:
        /*
         * 内核返回 -EBUSY：任务正在执行中
         * 先查询状态再给出友好提示
         */
        {
            int state = qos_status(qid);
            if (state == QOS_STATE_RUNNING || state == QOS_STATE_MERGING)
                fprintf(stderr,
                        "error: qid=%d is RUNNING, cannot cancel\n", qid);
            else
                fprintf(stderr,
                        "error: qid=%d cancel failed (state=%s)\n",
                        qid, qos_state_str(state));
        }
        return 1;

    default:
        fprintf(stderr, "error: qid=%d cancel failed: %s\n",
                qid, qos_err_str(ret));
        return 1;
    }
}