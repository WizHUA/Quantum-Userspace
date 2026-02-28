#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qstat <qid>\n"
            "       qstat -a\n"
            "       qstat --json\n"
            "\n"
            "Options:\n"
            "  <qid>    查询指定任务状态\n"
            "  -a       查询所有后端资源状态\n"
            "  --json   JSON格式输出\n"
            "  -h       帮助\n");
}

int main(int argc, char *argv[])
{
    int opt_all  = 0;
    int opt_json = 0;
    int qid      = 0;
    int i;

    if (argc < 2) { usage(); return 1; }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(); return 0;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_all = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            opt_json = 1;
        } else if (argv[i][0] != '-') {
            qid = atoi(argv[i]);
        }
    }

    /* ── -a 模式：显示所有后端 ── */
    if (opt_all) {
        qos_backend_pool_t *pool = calloc(1, sizeof(*pool));
        int ret;

        if (!pool) return 1;

        ret = qos_resource(pool);
        if (ret != QOS_OK) {
            fprintf(stderr, "qstat: resource query failed: %s\n",
                    qos_err_str(ret));
            free(pool);
            return 1;
        }

        if (opt_json) {
            char *buf = malloc(8192);
            if (buf) {
                int n = qos_backend_pool_to_json(pool, buf, 8192);
                if (n > 0) printf("%s", buf);
                free(buf);
            }
        } else {
            /* 表格：含 fidelity 列 */
            printf("%-10s  %-6s  %-14s  %-8s  %s\n",
                   "backend", "qubits", "state", "fidelity", "current_qid");
            printf("──────────────────────────────────────────────────────\n");
            for (i = 0; i < pool->num_backends; i++) {
                qos_backend_t *b = &pool->backends[i];
                char fid_str[16], qid_str[16];

                if (b->fidelity_score > 0)
                    snprintf(fid_str, sizeof(fid_str), "%d", b->fidelity_score);
                else
                    snprintf(fid_str, sizeof(fid_str), "-");

                if (b->current_qid >= 0)
                    snprintf(qid_str, sizeof(qid_str), "%d", b->current_qid);
                else
                    snprintf(qid_str, sizeof(qid_str), "-");

                printf("%-10s  %-6d  %-14s  %-8s  %s\n",
                       b->name, b->total_qubits,
                       qos_backend_state_str(b->state),
                       fid_str, qid_str);
            }
        }

        free(pool);
        return 0;
    }

    /* ── 单任务状态查询 ── */
    if (qid <= 0) {
        fprintf(stderr, "qstat: invalid qid\n");
        usage();
        return 1;
    }

    {
        int state = qos_status(qid);

        if (state < 0) {
            fprintf(stderr, "qstat: query failed: %s\n",
                    qos_err_str(state));
            return 1;
        }

        if (opt_json) {
            printf("{\"qid\": %d, \"state\": %d, \"state_str\": \"%s\"}\n",
                   qid, state, qos_state_str(state));
        } else {
            printf("qid=%-6d  state=%s\n", qid, qos_state_str(state));
        }
    }

    return 0;
}