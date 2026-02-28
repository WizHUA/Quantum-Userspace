#ifndef LIBQUANTUM_H
#define LIBQUANTUM_H

#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* 设备路径                                                             */
/* ------------------------------------------------------------------ */
#define QUANTUM_DEV_PATH        "/dev/quantum"

/* ------------------------------------------------------------------ */
/* 常量（与内核 quantum_types.h 严格对应）                              */
/* ------------------------------------------------------------------ */
#define QOS_MAX_QUBITS          64
#define QOS_MAX_TASKS           256
#define QOS_QIR_SIZE            4096
#define QOS_MAX_OUTCOMES        32
#define QOS_KEY_LEN             192
#define QOS_MAX_BACKENDS        8
#define QOS_MAX_SUB_CIRCUITS    8

/* ------------------------------------------------------------------ */
/* 任务状态码（对应内核 QTASK_STATE_*）                                 */
/* ------------------------------------------------------------------ */
#define QOS_STATE_UNKNOWN       0
#define QOS_STATE_RECEIVED      1
#define QOS_STATE_QUEUED        2
#define QOS_STATE_RUNNING       3
#define QOS_STATE_SUCCESS       4
#define QOS_STATE_FAILED        5
#define QOS_STATE_CANCELLED     6
#define QOS_STATE_MERGING       7

/* ------------------------------------------------------------------ */
/* 后端状态码（对应内核 QBACKEND_STATE_*）                              */
/* ------------------------------------------------------------------ */
#define QOS_BACKEND_IDLE        0
#define QOS_BACKEND_BUSY        1
#define QOS_BACKEND_CALIBRATING 2
#define QOS_BACKEND_OFFLINE     3

/* ------------------------------------------------------------------ */
/* 策略常量                                                             */
/* ------------------------------------------------------------------ */
#define QOS_ALLOC_FIRST_FIT     0
#define QOS_ALLOC_FIDELITY      1
#define QOS_ALLOC_REGRESSION    2
#define QOS_ALLOC_TOPO          3

#define QOS_SPLIT_NONE          0
#define QOS_SPLIT_SPACE_NAIVE   1
#define QOS_SPLIT_TIME          2
#define QOS_SPLIT_SPACE_PROB    3
#define QOS_SPLIT_TOPO_AWARE    4

#define QOS_MITI_NONE           0
#define QOS_MITI_MEM            1
#define QOS_MITI_CDR            2
#define QOS_MITI_PEC            3

/* ------------------------------------------------------------------ */
/* API 错误码                                                           */
/* ------------------------------------------------------------------ */
#define QOS_OK                  0
#define QOS_ERR_OPEN_DEV        (-1)
#define QOS_ERR_SUBMIT          (-2)
#define QOS_ERR_IOCTL           (-3)
#define QOS_ERR_TIMEOUT         (-4)
#define QOS_ERR_NOT_FOUND       (-5)
#define QOS_ERR_ARGS            (-6)
#define QOS_ERR_NOMEM           (-7)
#define QOS_ERR_KERN            (-8)

/* ------------------------------------------------------------------ */
/* 用户态数据结构                                                        */
/* ------------------------------------------------------------------ */

/* 提交配置 */
typedef struct {
    int shots;
    int priority;
    int error_mitigation;
    int alloc_strategy;
    int split_strategy;
} qos_config_t;

/* 执行结果 */
typedef struct {
    int  qid;
    int  shots;
    int  num_outcomes;
    char keys[QOS_MAX_OUTCOMES][QOS_KEY_LEN];
    int  counts[QOS_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];
    int  fidelity_score;
    int  num_sub_circuits;
} qos_result_t;

/* 单个后端描述 */
typedef struct {
    int  id;
    char name[32];
    int  total_qubits;
    int  state;
    int  current_qid;
    int  fidelity_score;
    int  num_qubits_available;
    int  connectivity_type;
} qos_backend_t;

/* 后端资源池 */
typedef struct {
    int          num_backends;
    qos_backend_t backends[QOS_MAX_BACKENDS];
} qos_backend_pool_t;

/* ------------------------------------------------------------------ */
/* 公开 API 声明                                                        */
/* ------------------------------------------------------------------ */

/* 核心 API */
int qos_submit(const char *circuit, const qos_config_t *config);
int qos_status(int qid);
int qos_cancel(int qid);
int qos_result(int qid, qos_result_t *out, int timeout_s);
int qos_resource(qos_backend_pool_t *out);

/* 输出辅助 API */
const char *qos_state_str(int state);
const char *qos_backend_state_str(int state);
const char *qos_err_str(int err);
void        qos_result_print(const qos_result_t *r);
void        qos_result_print_histogram(const qos_result_t *r);
int         qos_result_to_json(const qos_result_t *r, char *buf, int buf_size);
void        qos_backend_pool_print(const qos_backend_pool_t *pool);
int         qos_backend_pool_to_json(const qos_backend_pool_t *pool,
                                      char *buf, int buf_size);

#endif /* LIBQUANTUM_H */