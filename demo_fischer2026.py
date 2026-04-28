#!/usr/bin/env python3
"""demo_fischer2026.py — noiseless reference baseline for the Fischer-2026
4-qubit, 2-step kicked-Ising mini-slice.

Computes <Z_0 Z_3> exactly via Qiskit Aer statevector simulation. The C
demo (demo_fischer2026.c) prints reconstructed value vs hard-coded
baseline; this script lets the operator regenerate that baseline
on-demand. Kept tiny on purpose — the demo binary does NOT depend on
this; the kernel's quasi-prob postproc is the deliverable.

Usage: python3 expsh/userspace/demo_fischer2026.py
"""
from __future__ import annotations

import math
import sys


def baseline_z0z3() -> float:
    """Try Qiskit Aer; fall back to hard-coded literature value if unavailable."""
    try:
        from qiskit import QuantumCircuit
        from qiskit_aer import AerSimulator
    except Exception as exc:  # pragma: no cover - optional dep
        print(f"[demo.py] qiskit/aer not available ({exc}); "
              "returning literature placeholder")
        return -0.435

    qc = QuantumCircuit(4)
    for q in range(4):
        qc.h(q)
    for _ in range(2):
        qc.rzz(0.5, 0, 1)
        qc.rzz(0.5, 2, 3)
        qc.rzz(0.5, 1, 2)
        for q in range(4):
            qc.rx(1.0, q)
    qc.save_statevector()
    sim = AerSimulator(method="statevector")
    job = sim.run(qc)
    sv = job.result().get_statevector(qc)

    # <Z_0 Z_3> = sum_x |<x|psi>|^2 * (-1)^(x_0 XOR x_3)
    expect = 0.0
    for idx, amp in enumerate(sv):
        b0 = (idx >> 0) & 1
        b3 = (idx >> 3) & 1
        sign = -1 if (b0 ^ b3) else 1
        expect += sign * (amp.real ** 2 + amp.imag ** 2)
    return expect


def main() -> int:
    val = baseline_z0z3()
    print(f"[demo.py] noiseless <Z_0 Z_3> baseline = {val:+.4f}")
    print(f"[demo.py] update demo_fischer2026.c baseline literal "
          f"to {val:.3f} when ready")
    return 0


if __name__ == "__main__":
    sys.exit(main())
