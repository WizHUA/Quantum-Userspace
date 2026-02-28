#ifndef _LIBQUANTUM_H
#define _LIBQUANTUM_H

#include <stdint.h>

/* ===== 常量 ===== */
#define QUANTUM_DEV_PATH        "/dev/quantum"
#define QUANTUM_MAX_BACKENDS    8
#define QUANTUM_MAX_OUTCOMES    32
#define QUANTUM_KEY_LEN         192

/* ===== 任务状态码 ===== */
#define QTASK_STATE_UNKNOWN     0
#define QTASK_STATE_RECEIVED    1
#define QTASK_STATE_QUEUED      2
#define QTASK_STATE_RUNNING     3
#define QTASK_STATE_SUCCESS     4
#define QTASK_STATE_FAILED      5
#define QTASK_STATE_CANCELLED   6
#define QTASK_STATE_MERGING     7

/* ===== 后端状态码 ===== */
#define QBACKEND_STATE_IDLE         0
#define QBACKEND_STATE_BUSY         1
#define QBACKEND_STATE_CALIBRATING  2
#define QBACKEND_STATE_OFFLINE      3

/* ===== 错误码 ===== */
#define QERR_OK             0
#define QERR_OPEN_DEV      -1
#define QERR_SUBMIT        -2
#define QERR_STATUS        -3
#define QERR_RESULT        -4
#define QERR_RESOURCE      -5
#define QERR_NOT_FOUND     -6
#define QERR_TIMEOUT       -7
#define QERR_UNKNOWN       -8

/* ===== 提交配置 ===== */
typedef struct {
    int shots;              /* 测量次数，默认1000 */
    int priority;           /* 优先级 0~9，默认0 */
    int error_mitigation;   /* 误差缓解等级 0=关闭 */
} qos_config_t;

/* ===== 后端描述 ===== */
typedef struct {
    int  id;
    char name[32];
    int  total_qubits;
    int  state;
    int  current_qid;
} qos_backend_t;

/* ===== 后端池 ===== */
typedef struct {
    qos_backend_t backends[QUANTUM_MAX_BACKENDS];
    int           num_backends;
} qos_backend_pool_t;

/* ===== 执行结果 ===== */
typedef struct {
    int  qid;
    int  shots;
    int  num_outcomes;
    char keys[QUANTUM_MAX_OUTCOMES][QUANTUM_KEY_LEN];
    int  counts[QUANTUM_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];
} qos_result_t;

/* ===== API ===== */

/*
 * 提交量子线路
 * circuit: QASM字符串或文件路径
 * config:  执行配置，传NULL使用默认值
 * 返回值:  成功返回 qid（>0），失败返回负数错误码
 */
int qos_exec(const char *circuit, const qos_config_t *config);

/*
 * 查询任务状态
 * 返回值: QTASK_STATE_* 状态码，失败返回负数错误码
 */
int qos_status(int qid);

/*
 * 取回执行结果（阻塞直到任务完成或超时）
 * timeout_s: 超时秒数，0表示不超时
 * 返回值: QERR_OK 成功，其他为错误码
 */
int qos_result(int qid, qos_result_t *result, int timeout_s);

/*
 * 查询后端资源状态
 * 返回值: QERR_OK 成功，其他为错误码
 */
int qos_resource(qos_backend_pool_t *pool);

/*
 * 取消一个任务
 * 返回值: QERR_OK 成功，其他为错误码
 */
int qos_cancel(int qid);

/* ===== 工具函数 ===== */
const char *qos_state_str(int state);
const char *qos_backend_state_str(int state);
void        qos_result_print(const qos_result_t *result);

#endif /* _LIBQUANTUM_H */