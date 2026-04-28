/* demo_fischer2026.c — Fischer-2026 4-qubit kicked-Ising mini-slice demo
 *
 * Submits a 4-qubit, 2-Trotter-step kicked-Ising circuit through the
 * quantum_os kernel module with cut=WIRE em=READOUT, polls QIOC_STATUS
 * until SUCCESS, then reads QIOC_RESULT and prints
 *   [demo] expectation = X.XXX (baseline Y.YYY, diff Z%)
 *
 * The reconstructed expectation comes from the kernel's quasi-prob
 * postproc (see drivers/misc/quantum_os/quantum_postproc.c
 * merge_quasi_prob), encoded as result.counts[0] in millis with key
 * "Z0Zn_x1000". Baseline is a hard-coded Fischer-flavoured value
 * (placeholder — real noiseless reference owed to upstream).
 *
 * Build: gcc -Wall -O2 expsh/userspace/demo_fischer2026.c -o expsh/userspace/demo_fischer2026
 * Run:   sudo expsh/userspace/demo_fischer2026
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/ioctl.h>

#define QUANTUM_ABI_VERSION 3
#define QUANTUM_QIR_SIZE    4096
#define QOS_KEY_LEN         192   /* must match kernel QUANTUM_KEY_LEN */
#define QOS_MAX_OUTCOMES    64    /* must match kernel QUANTUM_MAX_OUTCOMES */

#define QIOC_MAGIC          'Q'
#define QIOC_SUBMIT         _IO(QIOC_MAGIC, 1)
#define QIOC_STATUS         _IO(QIOC_MAGIC, 2)
#define QIOC_RESULT         _IO(QIOC_MAGIC, 3)

#define QMIT_MEM            1
#define QCUT_WIRE           1
#define QALLOC_FIRST_FIT    0

#define QSTATE_SUCCESS      4
#define QSTATE_FAILED       5
#define QSTATE_CANCELLED    6

struct sub_req {
    __u32   abi_version;
    int     priority;
    int     shots;
    int     error_mitigation;
    int     alloc_strategy;
    int     cut_hint;
    char    qasm[QUANTUM_QIR_SIZE];
    int     qid;
};

struct sta_req {
    int qid;
    int state;
};

struct res_inner {
    int  shots;
    int  num_outcomes;
    char keys[QOS_MAX_OUTCOMES][QOS_KEY_LEN];
    int  counts[QOS_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];
};

struct res_req {
    int                qid;
    struct res_inner   result;
};

/*
 * 4-qubit, 2-step kicked-Ising Trotter slice (J=0.5, h=1.0).
 * Even/odd ZZ split (1D NN), then transverse RX kick, repeated.
 *
 * Uses cx-rz-cx decomposition for ZZ since qelib1.inc has no rzz:
 *    rzz(t) a,b  ==  cx a,b ; rz(t) b ; cx a,b
 */
static const char *kFischerQasm =
    "OPENQASM 2.0;\n"
    "include \"qelib1.inc\";\n"
    "qreg q[4];\n"
    "creg c[4];\n"
    "h q[0];\n h q[1];\n h q[2];\n h q[3];\n"
    /* step 1: ZZ on (0,1), (2,3), (1,2) */
    "cx q[0],q[1];\n rz(0.5) q[1];\n cx q[0],q[1];\n"
    "cx q[2],q[3];\n rz(0.5) q[3];\n cx q[2],q[3];\n"
    "cx q[1],q[2];\n rz(0.5) q[2];\n cx q[1],q[2];\n"
    "rx(1.0) q[0];\n rx(1.0) q[1];\n rx(1.0) q[2];\n rx(1.0) q[3];\n"
    /* step 2 */
    "cx q[0],q[1];\n rz(0.5) q[1];\n cx q[0],q[1];\n"
    "cx q[2],q[3];\n rz(0.5) q[3];\n cx q[2],q[3];\n"
    "cx q[1],q[2];\n rz(0.5) q[2];\n cx q[1],q[2];\n"
    "rx(1.0) q[0];\n rx(1.0) q[1];\n rx(1.0) q[2];\n rx(1.0) q[3];\n"
    "measure q -> c;\n";

static int open_dev(void)
{
    int fd = open("/dev/quantum", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[demo] open /dev/quantum failed: %s\n",
                strerror(errno));
    }
    return fd;
}

static int poll_status(int qid, int timeout_s)
{
    struct sta_req sr;
    int  fd, rc, elapsed_ms = 0;
    int  interval_ms = 50;
    int  timeout_ms  = timeout_s * 1000;
    struct timespec ts;
    while (1) {
        fd = open_dev();
        if (fd < 0) return -1;
        memset(&sr, 0, sizeof(sr));
        sr.qid = qid;
        rc = ioctl(fd, QIOC_STATUS, &sr);
        close(fd);
        if (rc == 0) {
            if (sr.state == QSTATE_SUCCESS)   return QSTATE_SUCCESS;
            if (sr.state == QSTATE_FAILED)    return QSTATE_FAILED;
            if (sr.state == QSTATE_CANCELLED) return QSTATE_CANCELLED;
        } else if (errno != ENOENT) {
            fprintf(stderr, "[demo] QIOC_STATUS errno=%d\n", errno);
            return -1;
        }
        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr, "[demo] poll timeout after %dms\n", elapsed_ms);
            return -1;
        }
        ts.tv_sec  = 0;
        ts.tv_nsec = (long)interval_ms * 1000000L;
        nanosleep(&ts, NULL);
        elapsed_ms += interval_ms;
        if (interval_ms < 400) interval_ms *= 2;
    }
}

int main(void)
{
    struct sub_req *sub;
    struct res_req *res;
    int   fd, rc, qid, st;
    int   recon_x1000 = 0;
    double recon, baseline = -0.435, diff_pct;
    int   i, found = 0;

    sub = calloc(1, sizeof(*sub));
    res = calloc(1, sizeof(*res));
    if (!sub || !res) { perror("calloc"); return 1; }

    sub->abi_version      = QUANTUM_ABI_VERSION;
    sub->priority         = 0;
    sub->shots            = 4096;
    sub->error_mitigation = QMIT_MEM;
    sub->alloc_strategy   = QALLOC_FIRST_FIT;
    sub->cut_hint         = QCUT_WIRE;
    strncpy(sub->qasm, kFischerQasm, QUANTUM_QIR_SIZE - 1);

    fd = open_dev();
    if (fd < 0) { free(sub); free(res); return 2; }

    rc = ioctl(fd, QIOC_SUBMIT, sub);
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "[demo] QIOC_SUBMIT failed: %s\n", strerror(errno));
        free(sub); free(res);
        return 3;
    }
    qid = sub->qid;
    printf("[demo] submitted Fischer-2026 4q/2-step trotter qid=%d "
           "cut=WIRE em=READOUT shots=%d\n", qid, sub->shots);
    printf("[demo] expect dmesg: preproc frags=2 wire_basis=6 em_var=4 "
           "variants_total=48 (clamped to manifest cap)\n");

    st = poll_status(qid, 30);
    if (st != QSTATE_SUCCESS) {
        fprintf(stderr, "[demo] task did not succeed (state=%d)\n", st);
        free(sub); free(res);
        return 4;
    }

    fd = open_dev();
    if (fd < 0) { free(sub); free(res); return 5; }
    res->qid = qid;
    rc = ioctl(fd, QIOC_RESULT, res);
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "[demo] QIOC_RESULT failed: %s\n", strerror(errno));
        free(sub); free(res);
        return 6;
    }

    printf("[demo] result qid=%d shots=%d num_outcomes=%d err=%d\n",
           res->qid, res->result.shots, res->result.num_outcomes,
           res->result.error_code);

    for (i = 0; i < res->result.num_outcomes && i < QOS_MAX_OUTCOMES; i++) {
        printf("[demo]   outcome[%d] key=%s count=%d\n",
               i, res->result.keys[i], res->result.counts[i]);
        if (!found && strncmp(res->result.keys[i], "Z0Z", 3) == 0) {
            recon_x1000 = res->result.counts[i];
            found = 1;
        }
    }

    if (!found) {
        fprintf(stderr, "[demo] no Z0Zn_x1000 outcome found; "
                "postproc may have fallen back to tensor merge\n");
        free(sub); free(res);
        return 7;
    }

    recon    = (double)recon_x1000 / 1000.0;
    diff_pct = (baseline != 0.0)
             ? ((recon - baseline) / baseline) * 100.0
             : 0.0;
    if (diff_pct < 0) diff_pct = -diff_pct;

    printf("[demo] expectation = %+.3f (baseline %+.3f, diff %.1f%%)\n",
           recon, baseline, diff_pct);

    free(sub); free(res);
    return 0;
}
