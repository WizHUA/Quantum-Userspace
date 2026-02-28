#include <stdio.h>
#include <stdlib.h>
#include "libquantum.h"

int main(void)
{
    qos_backend_pool_t *pool;
    int i, ret;

    pool = calloc(1, sizeof(*pool));
    if (!pool) return 1;

    ret = qos_resource(pool);
    if (ret != QERR_OK) {
        fprintf(stderr, "qresource: query failed (err=%d)\n", ret);
        free(pool);
        return 1;
    }

    printf("QuantumOS backend pool  (%d backends)\n",
           pool->num_backends);
    printf("──────────────────────────────────────────\n");
    printf("%-8s  %-6s  %-12s  %-10s\n",
           "name", "qubits", "state", "current_qid");
    printf("──────────────────────────────────────────\n");

    for (i = 0; i < pool->num_backends; i++) {
        qos_backend_t *b = &pool->backends[i];
        printf("%-8s  %-6d  %-12s  %d\n",
               b->name,
               b->total_qubits,
               qos_backend_state_str(b->state),
               b->current_qid);
    }

    free(pool);
    return 0;
}