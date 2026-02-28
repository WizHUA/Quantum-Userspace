#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include "libquantum.h"

/* ------------------------------------------------------------------ */
/* 内核结构体镜像（必须与 quantum_types.h 完全一致）                    */
/* ------------------------------------------------------------------ */

/* 对应内核 struct quantum_status_req */
struct kern_status_req {
    int qid;
    int state;
};

/* 对应内核 struct quantum_cancel_req */
struct kern_cancel_req {
    int qid;
};

/* 对应内核 struct quantum_result（嵌套子结构体，不含 qid）*/
struct kern_result_inner {
    int  shots;
    int  num_outcomes;
    char keys[QOS_MAX_OUTCOMES][QOS_KEY_LEN];
    int  counts[QOS_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];
};

/* 对应内核 struct quantum_result_req */
struct kern_result {
    int                      qid;
    struct kern_result_inner result;
};

/*
 * 对应内核 struct quantum_backend
 * 注意：last_calibration_time 是 __u64，紧跟 int state 和 int current_qid，
 * 编译器会在 current_qid(4字节) 后插入 4字节 padding 对齐 __u64
 */
struct kern_backend {
    int      id;
    char     name[32];
    int      total_qubits;
    int      state;
    int      current_qid;
    int      _pad;                  /* 对齐 last_calibration_time */
    uint64_t last_calibration_time;
    int      fidelity_score;
    int      num_qubits_available;
    int      connectivity_type;
};

/* 对应内核 struct quantum_backend_pool */
struct kern_backend_pool {
    struct kern_backend backends[QOS_MAX_BACKENDS];
    int                 num_backends;
};

/* ------------------------------------------------------------------ */
/* ioctl 命令码（与内核 quantum_types.h 严格一致）                      */
/* ------------------------------------------------------------------ */
#define QIOC_MAGIC      'Q'
#define QIOC_STATUS     _IO(QIOC_MAGIC, 2)
#define QIOC_RESULT     _IO(QIOC_MAGIC, 3)
#define QIOC_CANCEL     _IO(QIOC_MAGIC, 4)
#define QIOC_RESOURCE   _IO(QIOC_MAGIC, 5)

/* ------------------------------------------------------------------ */
/* 内部工具函数                                                         */
/* ------------------------------------------------------------------ */

static int open_dev(void)
{
    int fd = open(QUANTUM_DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "libquantum: cannot open %s: %s\n",
                QUANTUM_DEV_PATH, strerror(errno));
        return QOS_ERR_OPEN_DEV;
    }
    return fd;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int backoff_next_ms(int cur)
{
    int next = cur * 2;
    return next > 500 ? 500 : next;
}

/* ------------------------------------------------------------------ */
/* 输出辅助 API                                                         */
/* ------------------------------------------------------------------ */

const char *qos_state_str(int state)
{
    switch (state) {
    case QOS_STATE_UNKNOWN:   return "UNKNOWN";
    case QOS_STATE_RECEIVED:  return "RECEIVED";
    case QOS_STATE_QUEUED:    return "QUEUED";
    case QOS_STATE_RUNNING:   return "RUNNING";
    case QOS_STATE_SUCCESS:   return "SUCCESS";
    case QOS_STATE_FAILED:    return "FAILED";
    case QOS_STATE_CANCELLED: return "CANCELLED";
    case QOS_STATE_MERGING:   return "MERGING";
    default:                  return "UNKNOWN";
    }
}

const char *qos_backend_state_str(int state)
{
    switch (state) {
    case QOS_BACKEND_IDLE:        return "IDLE";
    case QOS_BACKEND_BUSY:        return "BUSY";
    case QOS_BACKEND_CALIBRATING: return "CALIBRATING";
    case QOS_BACKEND_OFFLINE:     return "OFFLINE";
    default:                      return "UNKNOWN";
    }
}

const char *qos_err_str(int err)
{
    switch (err) {
    case QOS_OK:            return "OK";
    case QOS_ERR_OPEN_DEV:  return "cannot open device";
    case QOS_ERR_SUBMIT:    return "submit failed";
    case QOS_ERR_IOCTL:     return "ioctl failed";
    case QOS_ERR_TIMEOUT:   return "timeout";
    case QOS_ERR_NOT_FOUND: return "not found";
    case QOS_ERR_ARGS:      return "invalid arguments";
    case QOS_ERR_NOMEM:     return "out of memory";
    case QOS_ERR_KERN:      return "kernel error";
    default:                return "unknown error";
    }
}

void qos_result_print(const qos_result_t *r)
{
    int i;
    if (!r) return;

    printf("\nqid=%-4d  shots=%-6d  outcomes=%d\n",
           r->qid, r->shots, r->num_outcomes);
    if (r->error_code)
        printf("error_code=%d  info=%s\n", r->error_code, r->error_info);

    printf("%-*s  %8s  %7s\n", QOS_KEY_LEN - 1, "state", "count", "prob");
    for (i = 0; i < 72; i++) putchar('-');
    putchar('\n');

    for (i = 0; i < r->num_outcomes; i++) {
        double prob = r->shots > 0
                      ? (double)r->counts[i] / r->shots * 100.0
                      : 0.0;
        printf("|%-*s>  %8d  %6.1f%%\n",
               QOS_KEY_LEN - 2, r->keys[i], r->counts[i], prob);
    }
}

void qos_result_print_histogram(const qos_result_t *r)
{
    int i;
    const int MAX_BAR = 40;
    if (!r) return;

    printf("\nqid=%d  shots=%d  outcomes=%d\n",
           r->qid, r->shots, r->num_outcomes);

    for (i = 0; i < r->num_outcomes; i++) {
        double prob = r->shots > 0
                      ? (double)r->counts[i] / r->shots : 0.0;
        int j, bar_len = (int)(prob * MAX_BAR + 0.5);
        printf("|%.*s>  |", 16, r->keys[i]);
        for (j = 0; j < bar_len; j++)     putchar('#');
        for (j = bar_len; j < MAX_BAR; j++) putchar(' ');
        printf("| %6.1f%%  (%d)\n", prob * 100.0, r->counts[i]);
    }
}

int qos_result_to_json(const qos_result_t *r, char *buf, int buf_size)
{
    int n = 0, i;
    if (!r || !buf || buf_size <= 0) return -1;

    n += snprintf(buf + n, buf_size - n,
                  "{\"qid\":%d,\"shots\":%d,\"num_outcomes\":%d,"
                  "\"error_code\":%d,\"error_info\":\"%s\","
                  "\"outcomes\":[",
                  r->qid, r->shots, r->num_outcomes,
                  r->error_code, r->error_info);

    for (i = 0; i < r->num_outcomes && n < buf_size - 1; i++) {
        double prob = r->shots > 0
                      ? (double)r->counts[i] / r->shots : 0.0;
        n += snprintf(buf + n, buf_size - n,
                      "%s{\"state\":\"%s\",\"count\":%d,\"prob\":%.4f}",
                      i > 0 ? "," : "",
                      r->keys[i], r->counts[i], prob);
    }

    n += snprintf(buf + n, buf_size - n, "]}");
    return n;
}

void qos_backend_pool_print(const qos_backend_pool_t *pool)
{
    int i;
    if (!pool) return;

    printf("\n%-4s  %-32s  %-6s  %-12s  %-8s  %-6s\n",
           "ID", "Name", "Qubits", "State", "CurQID", "Fidelity");
    for (i = 0; i < 72; i++) putchar('-');
    putchar('\n');

    for (i = 0; i < pool->num_backends; i++) {
        const qos_backend_t *b = &pool->backends[i];
        printf("%-4d  %-32s  %-6d  %-12s  %-8d  %-6d\n",
               b->id, b->name, b->total_qubits,
               qos_backend_state_str(b->state),
               b->current_qid, b->fidelity_score);
    }
}

int qos_backend_pool_to_json(const qos_backend_pool_t *pool,
                              char *buf, int buf_size)
{
    int n = 0, i;
    if (!pool || !buf || buf_size <= 0) return -1;

    n += snprintf(buf + n, buf_size - n,
                  "{\"num_backends\":%d,\"backends\":[",
                  pool->num_backends);

    for (i = 0; i < pool->num_backends && n < buf_size - 1; i++) {
        const qos_backend_t *b = &pool->backends[i];
        n += snprintf(buf + n, buf_size - n,
                      "%s{\"id\":%d,\"name\":\"%s\","
                      "\"total_qubits\":%d,\"state\":\"%s\","
                      "\"current_qid\":%d,\"fidelity_score\":%d}",
                      i > 0 ? "," : "",
                      b->id, b->name, b->total_qubits,
                      qos_backend_state_str(b->state),
                      b->current_qid, b->fidelity_score);
    }

    n += snprintf(buf + n, buf_size - n, "]}");
    return n;
}

/* ------------------------------------------------------------------ */
/* 核心 API 实现                                                        */
/* ------------------------------------------------------------------ */

/*
 * qos_submit —— 提交量子线路
 *
 * 走 write()+read() 而非 ioctl：
 *   write(fd, header+qasm, len)   → 内核 quantum_write() 处理提交链
 *   read(fd, &qid, sizeof(int))   → 内核 quantum_read() 返回分配的 qid
 */
int qos_submit(const char *circuit, const qos_config_t *config)
{
    char  header[256];
    char *buf;
    size_t header_len, circuit_len, total_len;
    int    fd, qid;
    ssize_t n;
    qos_config_t default_cfg = {1000, 0, 0, 0, 0};

    if (!circuit)
        return QOS_ERR_ARGS;
    if (!config)
        config = &default_cfg;

    /*
     * 配置头格式必须与内核 parse_submit_header() 完全一致：
     * "shots=N priority=P mitigation=M alloc_strategy=A split_strategy=S\n"
     */
    snprintf(header, sizeof(header),
             "shots=%d priority=%d mitigation=%d "
             "alloc_strategy=%d split_strategy=%d\n",
             config->shots,
             config->priority,
             config->error_mitigation,
             config->alloc_strategy,
             config->split_strategy);

    header_len  = strlen(header);
    circuit_len = strlen(circuit);
    total_len   = header_len + circuit_len;

    if (total_len >= QOS_QIR_SIZE) {
        fprintf(stderr, "libquantum: circuit too large "
                "(%zu bytes, max %d)\n", total_len, QOS_QIR_SIZE - 1);
        return QOS_ERR_ARGS;
    }

    buf = calloc(1, total_len + 1);
    if (!buf)
        return QOS_ERR_NOMEM;

    memcpy(buf, header, header_len);
    memcpy(buf + header_len, circuit, circuit_len);

    fd = open_dev();
    if (fd < 0) {
        free(buf);
        return QOS_ERR_OPEN_DEV;
    }

    /* 提交：write() = 配置头 + QASM */
    n = write(fd, buf, total_len);
    free(buf);

    if (n < 0) {
        fprintf(stderr, "libquantum: write() failed: %s\n",
                strerror(errno));
        close(fd);
        return QOS_ERR_SUBMIT;
    }

    /* write() 后立即 read() 取回内核分配的 qid */
    qid = -1;
    n = read(fd, &qid, sizeof(qid));
    close(fd);

    if (n != (ssize_t)sizeof(qid) || qid <= 0) {
        fprintf(stderr, "libquantum: read qid failed "
                "(n=%zd qid=%d: %s)\n", n, qid, strerror(errno));
        return QOS_ERR_SUBMIT;
    }

    return qid;
}

int qos_status(int qid)
{
    struct kern_status_req req;
    int fd, ret;

    if (qid <= 0)
        return QOS_ERR_ARGS;

    fd = open_dev();
    if (fd < 0)
        return fd;

    memset(&req, 0, sizeof(req));
    req.qid = qid;

    ret = ioctl(fd, QIOC_STATUS, &req);
    close(fd);

    if (ret < 0) {
        if (errno == ENOENT)
            return QOS_STATE_UNKNOWN;
        fprintf(stderr, "libquantum: status ioctl failed: %s\n",
                strerror(errno));
        return QOS_ERR_IOCTL;
    }

    return req.state;
}

int qos_cancel(int qid)
{
    struct kern_cancel_req req;
    int fd, ret;

    if (qid <= 0)
        return QOS_ERR_ARGS;

    fd = open_dev();
    if (fd < 0)
        return fd;

    req.qid = qid;
    ret = ioctl(fd, QIOC_CANCEL, &req);
    close(fd);

    if (ret < 0) {
        if (errno == ENOENT) return QOS_ERR_NOT_FOUND;
        if (errno == EBUSY)  return QOS_ERR_IOCTL;
        fprintf(stderr, "libquantum: cancel ioctl failed: %s\n",
                strerror(errno));
        return QOS_ERR_IOCTL;
    }

    return QOS_OK;
}

int qos_result(int qid, qos_result_t *out, int timeout_s)
{
    struct kern_result *kr;
    int    state;
    int    elapsed_ms  = 0;
    int    interval_ms = 50;
    int    unknown_cnt = 0;
    int    timeout_ms  = timeout_s > 0 ? timeout_s * 1000 : 30000;
    int    fd, ret, i;

    if (qid <= 0 || !out)
        return QOS_ERR_ARGS;

    /* 轮询等待任务完成 */
    while (1) {
        state = qos_status(qid);

        if (state == QOS_STATE_SUCCESS)
            break;

        if (state == QOS_STATE_FAILED)
            break;

        if (state == QOS_STATE_CANCELLED) {
            fprintf(stderr, "libquantum: qid=%d was cancelled\n", qid);
            return QOS_ERR_NOT_FOUND;
        }

        if (state == QOS_STATE_UNKNOWN || state < 0) {
            unknown_cnt++;
            if (unknown_cnt >= 3) {
                fprintf(stderr, "libquantum: qid=%d not found\n", qid);
                return QOS_ERR_NOT_FOUND;
            }
        }
        /* 不重置 unknown_cnt：split 任务状态反复变化，非 UNKNOWN 不清零 */

        if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
            fprintf(stderr, "libquantum: qid=%d timeout after %dms\n",
                    qid, elapsed_ms);
            return QOS_ERR_TIMEOUT;
        }

        sleep_ms(interval_ms);
        elapsed_ms  += interval_ms;
        interval_ms  = backoff_next_ms(interval_ms);
    }

    /* 取回结果：堆分配，避免栈溢出 */
    kr = calloc(1, sizeof(*kr));
    if (!kr)
        return QOS_ERR_NOMEM;

    kr->qid = qid;

    fd = open_dev();
    if (fd < 0) {
        free(kr);
        return QOS_ERR_OPEN_DEV;
    }

    ret = ioctl(fd, QIOC_RESULT, kr);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "libquantum: result ioctl failed: %s\n",
                strerror(errno));
        free(kr);
        return QOS_ERR_IOCTL;
    }

    memset(out, 0, sizeof(*out));
    out->qid          = kr->qid;
    out->shots        = kr->result.shots;
    out->num_outcomes = kr->result.num_outcomes;
    out->error_code   = kr->result.error_code;
    snprintf(out->error_info, sizeof(out->error_info),
             "%s", kr->result.error_info);

    if (out->num_outcomes > QOS_MAX_OUTCOMES)
        out->num_outcomes = QOS_MAX_OUTCOMES;

    for (i = 0; i < out->num_outcomes; i++) {
        strncpy(out->keys[i], kr->result.keys[i], QOS_KEY_LEN - 1);
        out->keys[i][QOS_KEY_LEN - 1] = '\0';
        out->counts[i] = kr->result.counts[i];
    }

    free(kr);
    return QOS_OK;
}

/*
 * qos_resource —— 查询后端资源池
 *
 * 内核返回 struct quantum_backend_pool：
 *   backends[QOS_MAX_BACKENDS]（先）+ num_backends（后）
 * kern_backend 中有 __u64 last_calibration_time，需要对齐
 */
int qos_resource(qos_backend_pool_t *out)
{
    struct kern_backend_pool *pool;
    int fd, ret, i;

    if (!out)
        return QOS_ERR_ARGS;

    pool = calloc(1, sizeof(*pool));
    if (!pool)
        return QOS_ERR_NOMEM;

    fd = open_dev();
    if (fd < 0) {
        free(pool);
        return QOS_ERR_OPEN_DEV;
    }

    ret = ioctl(fd, QIOC_RESOURCE, pool);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "libquantum: resource ioctl failed: %s\n",
                strerror(errno));
        free(pool);
        return QOS_ERR_IOCTL;
    }

    memset(out, 0, sizeof(*out));
    out->num_backends = pool->num_backends;
    if (out->num_backends > QOS_MAX_BACKENDS)
        out->num_backends = QOS_MAX_BACKENDS;

    for (i = 0; i < out->num_backends; i++) {
        out->backends[i].id                 = pool->backends[i].id;
        out->backends[i].total_qubits       = pool->backends[i].total_qubits;
        out->backends[i].state              = pool->backends[i].state;
        out->backends[i].current_qid        = pool->backends[i].current_qid;
        out->backends[i].fidelity_score     = pool->backends[i].fidelity_score;
        out->backends[i].num_qubits_available = pool->backends[i].num_qubits_available;
        out->backends[i].connectivity_type  = pool->backends[i].connectivity_type;
        strncpy(out->backends[i].name, pool->backends[i].name,
                sizeof(out->backends[i].name) - 1);
    }

    free(pool);
    return QOS_OK;
}