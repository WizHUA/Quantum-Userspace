#!/usr/bin/env python3
"""
qos_daemon.py — QuantumOS 后端守护进程
职责：
  1. 轮询内核 QIOC_FETCH，取出 RUNNING 状态的任务
  2. 调用 Qiskit Aer 执行真实量子线路
  3. 通过 QIOC_COMMIT 把结果写回内核
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

# ===== 常量（必须与 quantum_types.h 完全一致）=====
QUANTUM_DEV_PATH     = "/dev/quantum"
QUANTUM_QIR_SIZE     = 4096
QUANTUM_MAX_OUTCOMES = 32      # 与内核保持一致，勿改
QUANTUM_KEY_LEN      = 96

QIOC_MAGIC  = ord('Q')
QIOC_FETCH  = (QIOC_MAGIC << 8) | 6
QIOC_COMMIT = (QIOC_MAGIC << 8) | 7

POLL_INTERVAL_S = 0.2

# ===== 日志配置 =====
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [qos_daemon] %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("qos_daemon")

# ===== ctypes 结构体（字段顺序/类型/大小必须与内核侧完全一致）=====

class QuantumFetchReq(ctypes.Structure):
    """
    对应 struct quantum_fetch_req（quantum_types.h）
    sizeof = 4128 bytes
    """
    _fields_ = [
        ("qid",              ctypes.c_int),
        ("shots",            ctypes.c_int),
        ("num_qubits",       ctypes.c_int),
        ("circuit_depth",    ctypes.c_int),
        ("error_mitigation", ctypes.c_int),
        ("qasm",             ctypes.c_char * QUANTUM_QIR_SIZE),  # 4096
        # 子线路信息（need_split=1 时有效）
        ("need_split",       ctypes.c_int),
        ("sub_index",        ctypes.c_int),
        ("num_sub_circuits", ctypes.c_int),
    ]

class QuantumCommitReq(ctypes.Structure):
    """
    对应 struct quantum_commit_req（quantum_types.h）
    sizeof = 1308 bytes
    """
    _fields_ = [
        ("qid",          ctypes.c_int),
        ("success",      ctypes.c_int),
        ("shots",        ctypes.c_int),
        ("num_outcomes", ctypes.c_int),
        ("keys",         (ctypes.c_char * QUANTUM_KEY_LEN) * QUANTUM_MAX_OUTCOMES),  # 32*32=1024
        ("counts",       ctypes.c_int * QUANTUM_MAX_OUTCOMES),          # 32*4=128
        ("error_code",   ctypes.c_int),
        ("error_info",   ctypes.c_char * 128),
        ("need_split",   ctypes.c_int),
        ("sub_index",    ctypes.c_int),
    ]

# ===== 启动时校验结构体大小 =====

def verify_struct_sizes():
    """
    对比 ctypes 结构体大小与内核侧预期值
    不匹配时直接退出，防止堆破坏
    """
    expected = {
        "QuantumFetchReq":  4128,
        "QuantumCommitReq": 3356,
    }
    ok = True
    for name, size in expected.items():
        actual = ctypes.sizeof(globals()[name])
        if actual != size:
            log.error("STRUCT SIZE MISMATCH: %s expected=%d actual=%d "
                      "→ update quantum_types.h or qos_daemon.py",
                      name, size, actual)
            ok = False
        else:
            log.debug("struct %s size=%d OK", name, actual)
    if not ok:
        sys.exit(1)

# ===== ioctl 封装 =====

def ioctl_fetch(fd):
    req = QuantumFetchReq()
    try:
        fcntl.ioctl(fd, QIOC_FETCH, req)
    except OSError as e:
        import errno
        if e.errno == errno.EAGAIN:
            return None
        log.error("ioctl FETCH failed: %s", e)
        return None
    if req.qid < 0:
        return None
    return req

def ioctl_commit(fd, commit):
    try:
        fcntl.ioctl(fd, QIOC_COMMIT, commit)
        return True
    except OSError as e:
        log.error("ioctl COMMIT failed: %s", e)
        return False

# ===== Qiskit Aer 执行 =====

_simulator = AerSimulator()

def run_circuit(qasm, shots):
    qc = qasm2_loads(qasm)
    if not any(isinstance(inst.operation,
               __import__('qiskit.circuit', fromlist=['Measure']).Measure)
               for inst in qc.data):
        qc.measure_all()
    job    = _simulator.run(qc, shots=shots)
    result = job.result()
    return result.get_counts()

def execute_task(req):
    commit       = QuantumCommitReq()
    commit.qid   = req.qid
    commit.shots = req.shots

    # 子线路信息透传
    commit.need_split = req.need_split
    commit.sub_index  = req.sub_index

    # 决定执行哪段 QASM：
    # need_split=0 → 执行原始 qasm
    # need_split=1 → 内核 FETCH 时已把对应子线路的 qasm 填入 req.qasm
    qasm_str = req.qasm.decode("utf-8", errors="replace").rstrip("\x00")

    log.info("executing qid=%d sub=%d/%d shots=%d qubits=%d",
             req.qid, req.sub_index + 1,
             req.num_sub_circuits if req.need_split else 1,
             req.shots, req.num_qubits)

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
                 req.qid, req.sub_index, dict(outcomes[:4]))

    except Exception as e:
        log.error("qid=%d sub=%d failed: %s", req.qid, req.sub_index, e)
        commit.success    = 0
        commit.error_code = 6
        commit.error_info = str(e)[:127].encode("utf-8")

    return commit

# ===== 主循环 =====

_running = True

def handle_signal(signum, frame):
    global _running
    log.info("received signal %d, shutting down...", signum)
    _running = False

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dev",      default=QUANTUM_DEV_PATH)
    parser.add_argument("--interval", type=float, default=POLL_INTERVAL_S)
    parser.add_argument("--verbose",  action="store_true")
    args = parser.parse_args()

    if args.verbose:
        log.setLevel(logging.DEBUG)

    signal.signal(signal.SIGINT,  handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    log.info("starting, device=%s interval=%.2fs", args.dev, args.interval)
    log.info("Aer backend: %s", _simulator.configuration().backend_name)

    # 启动前校验结构体大小，不匹配直接退出
    verify_struct_sizes()

    try:
        fd = open(args.dev, "rb+", buffering=0)
    except PermissionError:
        log.error("cannot open %s: permission denied", args.dev)
        sys.exit(1)
    except FileNotFoundError:
        log.error("cannot open %s: not found", args.dev)
        sys.exit(1)

    log.info("device opened, entering poll loop...")

    while _running:
        req = ioctl_fetch(fd.fileno())
        if req is None:
            time.sleep(args.interval)
            continue

        log.info("fetched qid=%d need_split=%d", req.qid, req.need_split)
        commit = execute_task(req)
        ioctl_commit(fd.fileno(), commit)

    fd.close()
    log.info("daemon stopped")

if __name__ == "__main__":
    main()