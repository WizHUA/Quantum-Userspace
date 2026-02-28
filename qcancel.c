#include <stdio.h>
#include <stdlib.h>
#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr, "Usage: qcancel <qid>\n");
}

int main(int argc, char *argv[])
{
    int qid, ret;

    if (argc != 2) {
        usage();
        return 1;
    }

    qid = atoi(argv[1]);
    if (qid <= 0) {
        fprintf(stderr, "qcancel: invalid qid\n");
        return 1;
    }

    ret = qos_cancel(qid);
    if (ret != QERR_OK) {
        fprintf(stderr, "qcancel: failed (err=%d)\n", ret);
        return 1;
    }

    printf("cancelled: qid=%d\n", qid);
    return 0;
}