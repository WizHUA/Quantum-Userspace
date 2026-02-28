#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libquantum.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: qresource\n"
            "       qresource --json\n"
            "\n"
            "  查询量子后端资源信息\n"
            "\n"
            "Options:\n"
            "  --json   JSON格式输出\n"
            "  -h       帮助\n");
}

int main(int argc, char *argv[])
{
    qos_backend_pool_t *pool;
    int opt_json = 0;
    int ret, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        if (strcmp(argv[i], "--json") == 0) opt_json = 1;
    }

    pool = calloc(1, sizeof(*pool));
    if (!pool) return 1;

    ret = qos_resource(pool);
    if (ret != QOS_OK) {
        fprintf(stderr, "qresource: query failed: %s\n", qos_err_str(ret));
        free(pool);
        return 1;
    }

    if (opt_json) {
        char *buf = malloc(8192);
        if (buf) {
            int n = qos_backend_pool_to_json(pool, buf, 8192);
            if (n > 0) printf("%s", buf);
            else fprintf(stderr, "qresource: JSON serialization failed\n");
            free(buf);
        }
    } else {
        qos_backend_pool_print(pool);
    }

    free(pool);
    return 0;
}