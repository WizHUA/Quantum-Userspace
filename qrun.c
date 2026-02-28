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
/* 内核结构体镜像（仅 qrun 内部使用）                                   */
/* ------------------------------------------------------------------ */

/* 对应内核 struct quantum_status_req */
struct qrun_status_req {
    int qid;
    int state;
};

/* 对应内核 struct quantum_result_req */
struct qrun_result_req {
    int qid;
    struct {
        int  shots;
        int  num_outcomes;
        char keys[QOS_MAX_OUTCOMES][QOS_KEY_LEN];
        int  counts[QOS_MAX_OUTCOMES];
        int  error_code;
        char error_info[128];
    } result;
};

#define QIOC_STATUS  _IO('Q', 2)
#define QIOC_RESULT  _IO('Q', 3)

/* ------------------------------------------------------------------ */
/* 内部工具                                                             */
/* ------------------------------------------------------------------ */

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

static const char *state_name(int s)
{
    switch (s) {
    case QOS_STATE_QUEUED:    return "QUEUED";
    case QOS_STATE_RUNNING:   return "RUNNING";
    case QOS_STATE_MERGING:   return "MERGING";
    case QOS_STATE_SUCCESS:   return "SUCCESS";
    case QOS_STATE_FAILED:    return "FAILED";
    case QOS_STATE_CANCELLED: return "CANCELLED";
    default:                  return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* wait_with_progress：轮询状态并显示进度，返回 QOS_OK 或错误码        */
/* ------------------------------------------------------------------ */

static int wait_with_progress(int qid, int show_progress,
                               int timeout_s, qos_result_t *out)
{
    struct qrun_status_req sreq;
    struct qrun_result_req *rreq;
    int  fd;
    int  state;
    int  elapsed_ms   = 0;
    int  interval_ms  = 50;
    int  unknown_cnt  = 0;
    int  timeout_ms   = timeout_s > 0 ? timeout_s * 1000 : 60000;
    int  ret, i;
    char prog_buf[128];

    /* 轮询等待 */
    while (1) {
        fd = open(QUANTUM_DEV_PATH, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "qrun: cannot open %s: %s\n",
                    QUANTUM_DEV_PATH, strerror(errno));
            return QOS_ERR_OPEN_DEV;
        }

        memset(&sreq, 0, sizeof(sreq));
        sreq.qid = qid;
        ret = ioctl(fd, QIOC_STATUS, &sreq);
        close(fd);

        if (ret < 0) {
            state = (errno == ENOENT) ? QOS_STATE_UNKNOWN : -errno;
        } else {
            state = sreq.state;
        }

        if (show_progress) {
            snprintf(prog_buf, sizeof(prog_buf),
                     "\r[%-10s] %ds elapsed", state_name(state),
                     elapsed_ms / 1000);
            fputs(prog_buf, stdout);
            fflush(stdout);
        }

        if (state == QOS_STATE_SUCCESS)
            break;

        if (state == QOS_STATE_FAILED) {
            if (show_progress) putchar('\n');
            fprintf(stderr, "qrun: qid=%d FAILED\n", qid);
            break;
        }

        if (state == QOS_STATE_CANCELLED) {
            if (show_progress) putchar('\n');
            fprintf(stderr, "qrun: qid=%d was cancelled\n", qid);
            return QOS_ERR_NOT_FOUND;
        }

        if (state == QOS_STATE_UNKNOWN || state < 0) {
            unknown_cnt++;
            if (unknown_cnt >= 3) {
                if (show_progress) putchar('\n');
                fprintf(stderr, "qrun: qid=%d not found\n", qid);
                return QOS_ERR_NOT_FOUND;
            }
        }
        /* 修复：不重置 unknown_cnt
         * split 任务状态反复变化，非 UNKNOWN 不应清零计数 */

        if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
            if (show_progress) putchar('\n');
            fprintf(stderr, "qrun: qid=%d timeout\n", qid);
            return QOS_ERR_TIMEOUT;
        }

        sleep_ms(interval_ms);
        elapsed_ms  += interval_ms;
        interval_ms  = backoff_next_ms(interval_ms);
    }

    if (show_progress) putchar('\n');

    if (!out)
        return QOS_OK;

    /* 取回结果：堆分配，避免栈溢出 */
    rreq = calloc(1, sizeof(*rreq));
    if (!rreq)
        return QOS_ERR_NOMEM;

    rreq->qid = qid;

    fd = open(QUANTUM_DEV_PATH, O_RDWR);
    if (fd < 0) {
        free(rreq);
        return QOS_ERR_OPEN_DEV;
    }

    ret = ioctl(fd, QIOC_RESULT, rreq);
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "qrun: result ioctl failed: %s\n", strerror(errno));
        free(rreq);
        return QOS_ERR_IOCTL;
    }

    /* 修复：通过嵌套 result 字段访问，与内核 quantum_result_req 一致 */
    memset(out, 0, sizeof(*out));
    out->qid          = rreq->qid;
    out->shots        = rreq->result.shots;
    out->num_outcomes = rreq->result.num_outcomes;
    out->error_code   = rreq->result.error_code;
    snprintf(out->error_info, sizeof(out->error_info),
             "%s", rreq->result.error_info);

    if (out->num_outcomes > QOS_MAX_OUTCOMES)
        out->num_outcomes = QOS_MAX_OUTCOMES;

    for (i = 0; i < out->num_outcomes; i++) {
        strncpy(out->keys[i], rreq->result.keys[i], QOS_KEY_LEN - 1);
        out->keys[i][QOS_KEY_LEN - 1] = '\0';
        out->counts[i] = rreq->result.counts[i];
    }

    free(rreq);
    return QOS_OK;
}

/* ------------------------------------------------------------------ */
/* 打印结果表格                                                         */
/* ------------------------------------------------------------------ */

static void print_result(const qos_result_t *r)
{
    int i;
    printf("\nqid=%-4d  shots=%-6d  outcomes=%d\n",
           r->qid, r->shots, r->num_outcomes);
    if (r->error_code)
        printf("error_code=%d  info=%s\n", r->error_code, r->error_info);

    printf("%-*s  %8s  %7s\n", QOS_KEY_LEN - 1, "state", "count", "prob");
    for (i = 0; i < 60; i++) putchar('-');
    putchar('\n');
    for (i = 0; i < r->num_outcomes; i++) {
        double prob = r->shots > 0
                      ? (double)r->counts[i] / r->shots * 100.0
                      : 0.0;
        printf("|%-*s>  %8d  %6.1f%%\n",
               QOS_KEY_LEN - 2, r->keys[i], r->counts[i], prob);
    }
}

/* ------------------------------------------------------------------ */
/* 读取 QASM 文件                                                       */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path)
{
    FILE *f;
    long  sz;
    char *buf;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "qrun: cannot open file: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);

    if (sz <= 0 || sz >= QOS_QIR_SIZE) {
        fprintf(stderr, "qrun: file too large or empty (%ld bytes)\n", sz);
        fclose(f);
        return NULL;
    }

    buf = calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "qrun: read error\n");
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char  *qasm_file   = NULL;
    int          wait_flag   = 0;
    int          progress    = 0;
    int          timeout_s   = 60;
    int          shots       = 1000;
    int          qid;
    char        *circuit;
    qos_config_t cfg;
    qos_result_t result;
    int          ret;
    int          i;

    /* 参数解析 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            wait_flag = 1;
        } else if (strcmp(argv[i], "--progress") == 0) {
            progress = 1;
        } else if (strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
            shots = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            qasm_file = argv[i];
        }
    }

    if (!qasm_file) {
        fprintf(stderr,
                "Usage: qrun [options] <circuit.qasm>\n"
                "  -w            wait for result\n"
                "  --progress    show progress bar\n"
                "  --shots N     number of shots (default 1000)\n"
                "  --timeout N   timeout in seconds (default 60)\n");
        return 1;
    }

    /* 读取线路文件 */
    circuit = read_file(qasm_file);
    if (!circuit)
        return 1;

    /* 提交任务 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.shots = shots;

    qid = qos_submit(circuit, &cfg);
    free(circuit);

    if (qid < 0) {
        fprintf(stderr, "qrun: submit failed: %d\n", qid);
        return 1;
    }

    printf("submitted: qid=%d\n", qid);

    /* 等待结果 */
    if (wait_flag) {
        printf("waiting for result...\n");
        ret = wait_with_progress(qid, progress, timeout_s, &result);
        if (ret != QOS_OK) {
            fprintf(stderr, "qrun: wait failed: %d\n", ret);
            return 1;
        }
        print_result(&result);
    }

    return 0;
}