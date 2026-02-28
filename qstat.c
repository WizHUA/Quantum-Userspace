#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qstat <qid>\n"
            "       qstat -a\n"
            "\n"
            "Options:\n"
            "  <qid>   查询指定任务状态\n"
            "  -a      查询所有后端资源状态\n");
}

int main(int argc, char *argv[])
{
    int qid, state;

    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "-a") == 0) {
        /* 显示后端池状态 */
        qos_backend_pool_t *pool = calloc(1, sizeof(*pool));
        if (!pool) return 1;

        if (qos_resource(pool) == QERR_OK) {
            int i;
            printf("%-8s  %-6s  %-12s  %s\n",
                   "backend", "qubits", "state", "current_qid");
            printf("──────────────────────────────────────\n");
            for (i = 0; i < pool->num_backends; i++) {
                qos_backend_t *b = &pool->backends[i];
                printf("%-8s  %-6d  %-12s  %d\n",
                       b->name, b->total_qubits,
                       qos_backend_state_str(b->state),
                       b->current_qid);
            }
        }
        free(pool);
        return 0;
    }

    qid   = atoi(argv[1]);
    state = qos_status(qid);

    if (state < 0) {
        fprintf(stderr, "qstat: query failed (err=%d)\n", state);
        return 1;
    }

    printf("qid=%-6d  state=%d (%s)\n",
           qid, state, qos_state_str(state));
    return 0;
}