#!/usr/bin/env python3
"""
qos_daemon.py — QuantumOS 后端守护进程

职责：
  1. 轮询内核 QIOC_FETCH，取出 RUNNING 状态的任务（或子线路）
  2. 调用 Qiskit Aer 执行量子线路
  3. 通过 QIOC_COMMIT 把结果写回内核

设计约束：
  - daemon 是内核调度系统的"执行臂"，不做任何调度决策
  - QASM 由内核在 FETCH 时已剥离配置头，daemon 直接解析
  - 结构体大小必须与内核 quantum_types.h 严格一致，启动时自动校验
"""

import os
import sys
import time
import fcntl
import ctypes
import signal
import logging
import argparse

from qiskit import QuantumCircuit
from qiskit.qasm2 import loads as qasm2_loads
from qiskit_aer import AerSimulator

# ================================================================
# 常量（必须与内核 quantum_types.h 完全一致，不得单独修改）
# ================================================================

QUANTUM_DEV_PATH      = "/dev/quantum"
QUANTUM_QIR_SIZE      = 4096
QUANTUM_MAX_QUBITS    = 64
QUANTUM_MAX_OUTCOMES  = 32
QUANTUM_KEY_LEN       = 192

# ioctl 命令字：_IO('Q', N) = (ord('Q') << 8) | N
QIOC_MAGIC  = ord('Q')
QIOC_FETCH  = (QIOC_MAGIC << 8) | 6
QIOC_COMMIT = (QIOC_MAGIC << 8) | 7

# ================================================================
# 日志配置
# ================================================================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [qos_daemon] %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("qos_daemon")

# ================================================================
# ctypes 结构体定义
#
# 字段顺序/类型/大小必须与内核侧完全一致
# 修改内核结构体后必须同步修改以下定义，并更新 verify_struct_sizes() 中的期望值
# ================================================================

class QuantumFetchReq(ctypes.Structure):
    """
    对应内核 struct quantum_fetch_req
    内核 FETCH 时已剥离配置头，qasm 字段为纯 QASM
    """
    _fields_ = [
        ("qid",              ctypes.c_int),
        ("shots",            ctypes.c_int),
        ("num_qubits",       ctypes.c_int),
        ("circuit_depth",    ctypes.c_int),
        ("error_mitigation", ctypes.c_int),
        ("qasm",             ctypes.c_char * QUANTUM_QIR_SIZE),   # 4096 bytes
        ("need_split",       ctypes.c_int),
        ("sub_index",        ctypes.c_int),
        ("num_sub_circuits", ctypes.c_int),
        ("phys_qubits",      ctypes.c_int * QUANTUM_MAX_QUBITS),  # 64*4=256 bytes
    ]

class QuantumCommitReq(ctypes.Structure):
    """
    对应内核 struct quantum_commit_req
    """
    _fields_ = [
        ("qid",          ctypes.c_int),
        ("success",      ctypes.c_int),
        ("shots",        ctypes.c_int),
        ("num_outcomes", ctypes.c_int),
        ("keys",         (ctypes.c_char * QUANTUM_KEY_LEN) * QUANTUM_MAX_OUTCOMES),  # 32*192=6144
        ("counts",       ctypes.c_int * QUANTUM_MAX_OUTCOMES),                       # 32*4=128
        ("error_code",   ctypes.c_int),
        ("error_info",   ctypes.c_char * 128),
        ("need_split",   ctypes.c_int),
        ("sub_index",    ctypes.c_int),
    ]

# ================================================================
# 启动时结构体大小校验
# ================================================================

# 期望值（根据内核实际 sizeof 计算）：
#   QuantumFetchReq：5*4 + 4096 + 3*4 + 64*4 = 20 + 4096 + 12 + 256 = 4384
#   QuantumCommitReq：4*4 + 32*192 + 32*4 + 4 + 128 + 2*4 = 16+6144+128+4+128+8 = 6428
EXPECTED_FETCH_SIZE  = 4384
EXPECTED_COMMIT_SIZE = 6428

def verify_struct_sizes():
    """
    校验 ctypes 结构体大小是否与内核侧匹配
    不匹配时打印详细错误并直接退出，防止堆数据损坏
    """
    ok = True

    actual_fetch  = ctypes.sizeof(QuantumFetchReq)
    actual_commit = ctypes.sizeof(QuantumCommitReq)

    if actual_fetch != EXPECTED_FETCH_SIZE:
        log.error(
            "STRUCT SIZE MISMATCH: QuantumFetchReq "
            "expected=%d actual=%d — sync quantum_types.h or qos_daemon.py",
            EXPECTED_FETCH_SIZE, actual_fetch
        )
        ok = False
    else:
        log.debug("QuantumFetchReq size=%d OK", actual_fetch)

    if actual_commit != EXPECTED_COMMIT_SIZE:
        log.error(
            "STRUCT SIZE MISMATCH: QuantumCommitReq "
            "expected=%d actual=%d — sync quantum_types.h or qos_daemon.py",
            EXPECTED_COMMIT_SIZE, actual_commit
        )
        ok = False
    else:
        log.debug("QuantumCommitReq size=%d OK", actual_commit)

    if not ok:
        sys.exit(1)

# ================================================================
# ioctl 封装
# ================================================================

def ioctl_fetch(fd):
    """
    调用 QIOC_FETCH，取出待执行任务
    返回 QuantumFetchReq（有任务）或 None（无任务/EAGAIN）
    """
    req = QuantumFetchReq()
    try:
        fcntl.ioctl(fd, QIOC_FETCH, req)
    except OSError as e:
        import errno as errno_mod
        if e.errno == errno_mod.EAGAIN:
            return None  # 无待执行任务，正常情况
        log.warning("QIOC_FETCH failed: %s", e)
        return None
    if req.qid <= 0:
        return None
    return req

def ioctl_commit(fd, commit):
    """
    调用 QIOC_COMMIT，提交执行结果
    返回 True=成功，False=失败（内核侧会超时清理）
    """
    try:
        fcntl.ioctl(fd, QIOC_COMMIT, commit)
        return True
    except OSError as e:
        log.warning("QIOC_COMMIT failed: %s", e)
        return False

# ================================================================
# 量子线路执行
# ================================================================

_simulator = AerSimulator()

def run_circuit(qasm_str, shots):
    """
    使用 Qiskit Aer 执行量子线路

    qasm_str: 纯 QASM 字符串（内核 FETCH 时已剥离配置头）
    shots:    测量次数
    返回 counts dict，如 {'00': 512, '11': 488}
    """
    # 解析 QASM
    try:
        qc = qasm2_loads(qasm_str)
    except Exception as e:
        raise RuntimeError(f"QASM parse error: {e}") from e

    # 若无 measure 指令则自动补全 measure_all()
    from qiskit.circuit import Measure
    has_measure = any(
        isinstance(inst.operation, Measure) for inst in qc.data
    )
    if not has_measure:
        qc.measure_all()

    # 执行
    job    = _simulator.run(qc, shots=shots)
    result = job.result()
    return result.get_counts()

def execute_task(req):
    """
    执行一个 FETCH 到的任务（或子线路），返回填好的 QuantumCommitReq
    """
    commit            = QuantumCommitReq()
    commit.qid        = req.qid
    commit.shots      = req.shots
    commit.need_split = req.need_split
    commit.sub_index  = req.sub_index

    # 解码 QASM（内核已剥离配置头，直接使用）
    qasm_str = req.qasm.decode("utf-8", errors="replace").rstrip("\x00")

    total_subs = req.num_sub_circuits if req.need_split else 1
    log.info("executing qid=%d sub=%d/%d shots=%d qubits=%d",
             req.qid, req.sub_index + 1, total_subs,
             req.shots, req.num_qubits)
    log.debug("qasm=\n%s", qasm_str[:200])

    try:
        counts   = run_circuit(qasm_str, req.shots)
        outcomes = sorted(counts.items(), key=lambda x: -x[1])

        commit.success      = 1
        commit.num_outcomes = min(len(outcomes), QUANTUM_MAX_OUTCOMES)

        for i, (key, cnt) in enumerate(outcomes[:QUANTUM_MAX_OUTCOMES]):
            key_bytes = key.encode("utf-8")[:QUANTUM_KEY_LEN - 1]
            ctypes.memmove(commit.keys[i], key_bytes, len(key_bytes))
            commit.counts[i] = cnt

        log.info("qid=%d sub=%d done: %s",
                 req.qid, req.sub_index,
                 dict(list(outcomes)[:4]))

    except RuntimeError as e:
        # QASM 解析失败
        log.error("qid=%d sub=%d QASM error: %s",
                  req.qid, req.sub_index, e)
        commit.success    = 0
        commit.error_code = 4   # QERR_COMPILE_FAIL
        commit.error_info = str(e)[:127].encode("utf-8")

    except Exception as e:
        # Aer 执行失败
        log.error("qid=%d sub=%d execution error: %s",
                  req.qid, req.sub_index, e)
        commit.success    = 0
        commit.error_code = 6   # QERR_BACKEND_FAIL
        commit.error_info = str(e)[:127].encode("utf-8")

    return commit

# ================================================================
# 主循环
# ================================================================

_running = True

def handle_signal(signum, frame):
    global _running
    log.info("received signal %d, shutting down gracefully...", signum)
    _running = False

def open_device(path):
    """打开设备文件，返回文件对象，失败时返回 None"""
    try:
        return open(path, "rb+", buffering=0)
    except PermissionError:
        log.error("cannot open %s: permission denied "
                  "(try sudo or check /dev/quantum permissions)", path)
        return None
    except FileNotFoundError:
        log.error("cannot open %s: not found "
                  "(is quantum_os.ko loaded?)", path)
        return None
    except OSError as e:
        log.error("cannot open %s: %s", path, e)
        return None

def main():
    parser = argparse.ArgumentParser(
        description="QuantumOS backend daemon"
    )
    parser.add_argument("--dev",
                        default=QUANTUM_DEV_PATH,
                        help="设备文件路径（默认 /dev/quantum）")
    parser.add_argument("--interval",
                        type=float, default=0.5,
                        help="无任务时的轮询间隔（秒，默认 0.5）")
    parser.add_argument("--verbose",
                        action="store_true",
                        help="DEBUG 级别日志")
    args = parser.parse_args()

    if args.verbose:
        log.setLevel(logging.DEBUG)

    # 注册信号处理
    signal.signal(signal.SIGINT,  handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    log.info("starting qos_daemon (device=%s interval=%.2fs)",
             args.dev, args.interval)
    log.info("Aer backend: %s",
             _simulator.configuration().backend_name)

    # 启动时校验结构体大小（不匹配直接退出）
    verify_struct_sizes()

    # 打开设备
    f = open_device(args.dev)
    if f is None:
        sys.exit(1)

    fd = f.fileno()
    log.info("device opened (fd=%d), entering poll loop...", fd)

    # 主轮询循环
    while _running:
        req = ioctl_fetch(fd)

        if req is None:
            # 无待执行任务，等待后重试
            time.sleep(args.interval)
            continue

        log.info("fetched qid=%d need_split=%d sub=%d",
                 req.qid, req.need_split, req.sub_index)

        commit = execute_task(req)
        ok     = ioctl_commit(fd, commit)

        if ok:
            log.debug("committed qid=%d sub=%d success=%d",
                      commit.qid, commit.sub_index, commit.success)
        else:
            log.warning("commit failed for qid=%d sub=%d "
                        "(kernel will timeout and reclaim)",
                        commit.qid, commit.sub_index)

    f.close()
    log.info("daemon stopped")

if __name__ == "__main__":
    main()