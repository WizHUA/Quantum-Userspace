#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "libquantum.h"

/* ===== ioctl 命令字（与内核侧一致）===== */
#define QIOC_MAGIC      'Q'
#define QIOC_STATUS     _IO(QIOC_MAGIC, 2)
#define QIOC_RESULT     _IO(QIOC_MAGIC, 3)
#define QIOC_CANCEL     _IO(QIOC_MAGIC, 4)
#define QIOC_RESOURCE   _IO(QIOC_MAGIC, 5)

/* 内核侧结构体：与 quantum_types.h 保持一致 */
struct kern_result {
    int  qid;
    int  shots;
    int  num_outcomes;
    char keys[QUANTUM_MAX_OUTCOMES][96];
    int  counts[QUANTUM_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];
};

struct kern_backend {
    int      id;
    char     name[32];
    int      total_qubits;
    int      state;
    int      current_qid;
    uint64_t last_calibration_time;
};

struct kern_backend_pool {
    struct kern_backend backends[QUANTUM_MAX_BACKENDS];
    int                 num_backends;
};

/* ===== 内部工具：打开设备 ===== */
static int open_dev(void)
{
    int fd = open(QUANTUM_DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "libquantum: cannot open %s: %s\n",
                QUANTUM_DEV_PATH, strerror(errno));
        return QERR_OPEN_DEV;
    }
    return fd;
}

/* ===== API 实现 ===== */

int qos_exec(const char *circuit, const qos_config_t *config)
{
    static qos_config_t default_config = {
        .shots            = 1000,
        .priority         = 0,
        .error_mitigation = 0,
    };
    const qos_config_t *cfg = config ? config : &default_config;
    char buf[8192];
    int  fd, ret, qid;

    if (!circuit)
        return QERR_SUBMIT;

    fd = open_dev();
    if (fd < 0)
        return fd;

    snprintf(buf, sizeof(buf),
             "shots=%d priority=%d mitigation=%d\n%s",
             cfg->shots, cfg->priority, cfg->error_mitigation,
             circuit);

    /* 提交线路 */
    ret = write(fd, buf, strlen(buf));
    if (ret < 0) {
        fprintf(stderr, "libquantum: submit failed: %s\n",
                strerror(errno));
        close(fd);
        return QERR_SUBMIT;
    }

    /* 立即读回内核分配的真实 qid */
    ret = read(fd, &qid, sizeof(int));
    if (ret != sizeof(int)) {
        fprintf(stderr, "libquantum: failed to read qid: %s\n",
                strerror(errno));
        close(fd);
        return QERR_SUBMIT;
    }

    close(fd);
    return qid;   /* 返回真实 qid */
}

int qos_status(int qid)
{
    int fd, ret;

    fd = open_dev();
    if (fd < 0)
        return fd;

    ret = ioctl(fd, QIOC_STATUS, &qid);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "libquantum: status query failed: %s\n",
                strerror(errno));
        return QERR_STATUS;
    }
    return ret;
}

int qos_result(int qid, qos_result_t *out, int timeout_s)
{
    struct kern_result *kr;   /* 补充声明 */
    int fd, ret, i;           /* 补充声明 */
    int elapsed       = 0;
    int unknown_count = 0;

    if (!out)
        return QERR_RESULT;

    while (1) {
        ret = qos_status(qid);

        if (ret == QTASK_STATE_SUCCESS)
            break;

        if (ret == QTASK_STATE_FAILED) {
            fprintf(stderr, "libquantum: qid=%d failed\n", qid);
            return QERR_RESULT;
        }

        if (ret == QTASK_STATE_UNKNOWN) {
            unknown_count++;
            if (unknown_count >= 3) {
                fprintf(stderr, "libquantum: qid=%d not found\n", qid);
                return QERR_NOT_FOUND;
            }
        } else {
            unknown_count = 0;
        }

        if (timeout_s > 0 && elapsed >= timeout_s) {
            fprintf(stderr, "libquantum: qid=%d timeout\n", qid);
            return QERR_TIMEOUT;
        }
        sleep(1);
        elapsed++;
    }

    /* 取回结果 */
    kr = calloc(1, sizeof(*kr));
    if (!kr)
        return QERR_RESULT;

    kr->qid = qid;

    fd = open_dev();
    if (fd < 0) {
        free(kr);
        return fd;
    }

    ret = ioctl(fd, QIOC_RESULT, kr);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "libquantum: get result failed: %s\n",
                strerror(errno));
        free(kr);
        return QERR_RESULT;
    }

    /* 内核结构体 → 用户结构体 */
    out->qid          = kr->qid;
    out->shots        = kr->shots;
    out->num_outcomes = kr->num_outcomes;
    out->error_code   = kr->error_code;
    snprintf(out->error_info, sizeof(out->error_info),
             "%s", kr->error_info);
    for (i = 0; i < kr->num_outcomes && i < QUANTUM_MAX_OUTCOMES; i++) {
        strncpy(out->keys[i], kr->keys[i], sizeof(out->keys[i]) - 1);
        out->counts[i] = kr->counts[i];
    }

    free(kr);
    return QERR_OK;
}

int qos_resource(qos_backend_pool_t *out)
{
    struct kern_backend_pool *kp;
    int fd, ret, i;

    if (!out)
        return QERR_RESOURCE;

    kp = calloc(1, sizeof(*kp));
    if (!kp)
        return QERR_RESOURCE;

    fd = open_dev();
    if (fd < 0) {
        free(kp);
        return fd;
    }

    ret = ioctl(fd, QIOC_RESOURCE, kp);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "libquantum: resource query failed: %s\n",
                strerror(errno));
        free(kp);
        return QERR_RESOURCE;
    }

    /* 内核结构体 → 用户结构体 */
    out->num_backends = kp->num_backends;
    for (i = 0; i < kp->num_backends && i < QUANTUM_MAX_BACKENDS; i++) {
        out->backends[i].id           = kp->backends[i].id;
        out->backends[i].total_qubits = kp->backends[i].total_qubits;
        out->backends[i].state        = kp->backends[i].state;
        out->backends[i].current_qid  = kp->backends[i].current_qid;
        strncpy(out->backends[i].name, kp->backends[i].name,
                sizeof(out->backends[i].name));
    }

    free(kp);
    return QERR_OK;
}

int qos_cancel(int qid)
{
    int fd, ret;

    fd = open_dev();
    if (fd < 0)
        return fd;

    ret = ioctl(fd, QIOC_CANCEL, &qid);
    close(fd);

    if (ret < 0) {
        if (errno == ENOENT)
            return QERR_NOT_FOUND;
        return QERR_UNKNOWN;
    }
    return QERR_OK;
}

/* ===== 工具函数 ===== */

const char *qos_state_str(int state)
{
    switch (state) {
    case QTASK_STATE_UNKNOWN:   return "UNKNOWN";
    case QTASK_STATE_RECEIVED:  return "RECEIVED";
    case QTASK_STATE_QUEUED:    return "QUEUED";
    case QTASK_STATE_RUNNING:   return "RUNNING";
    case QTASK_STATE_SUCCESS:   return "SUCCESS";
    case QTASK_STATE_FAILED:    return "FAILED";
    case QTASK_STATE_CANCELLED: return "CANCELLED";
    case QTASK_STATE_MERGING:   return "MERGING";
    default:                    return "INVALID";
    }
}

const char *qos_backend_state_str(int state)
{
    switch (state) {
    case QBACKEND_STATE_IDLE:        return "IDLE";
    case QBACKEND_STATE_BUSY:        return "BUSY";
    case QBACKEND_STATE_CALIBRATING: return "CALIBRATING";
    case QBACKEND_STATE_OFFLINE:     return "OFFLINE";
    default:                         return "UNKNOWN";
    }
}

void qos_result_print(const qos_result_t *r)
{
    int i, total = 0;

    printf("qid=%d  shots=%d  outcomes=%d\n",
           r->qid, r->shots, r->num_outcomes);

    /* 动态计算 key 最大宽度，至少16字符 */
    int max_key_len = 16;
    for (i = 0; i < r->num_outcomes; i++) {
        int len = (int)strnlen(r->keys[i], QUANTUM_KEY_LEN);
        if (len > max_key_len)
            max_key_len = len;
    }

    /* 打印表头 */
    printf("%-*s  %8s  %8s\n", max_key_len + 2, "state", "count", "prob");
    for (i = 0; i < max_key_len + 22; i++) printf("─");
    printf("\n");

    for (i = 0; i < r->num_outcomes; i++)
        total += r->counts[i];
    if (total == 0) total = 1;

    for (i = 0; i < r->num_outcomes; i++) {
        int pct_int  = r->counts[i] * 100  / total;
        int pct_frac = r->counts[i] * 1000 / total % 10;

        /* |key> 完整打印，不截断 */
        printf("|%s>  %8d  %5d.%d%%\n",
               r->keys[i],
               r->counts[i],
               pct_int, pct_frac);
    }
}