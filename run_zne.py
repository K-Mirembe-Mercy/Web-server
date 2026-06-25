"""
run_zne.py
----------
Standalone ZNE benchmark runner.
Builds representative circuits for each algorithm at n=3 and n=5 qubits,
applies Zero-Noise Extrapolation, and generates three output plots.

Usage:
    python run_zne.py
"""

import os
import numpy as np
from algorithms.grovers import build_grover_circuit
from algorithms.deutsch_jozsa import build_dj_circuit
from algorithms.bernstein_vazirani import build_bv_circuit
from analysis import run_zne_suite, plot_noise_scaling_curves, \
                     plot_mitigation_comparison, plot_mitigation_gain

RESULTS_DIR = "results"
os.makedirs(RESULTS_DIR, exist_ok=True)


def build_suite() -> list[tuple]:
    """
    Returns a list of (circuit, target_bitstring, label) tuples
    covering each algorithm at two qubit counts.
    """
    suite = []
    for n in (3, 5):
        # Grover
        target = format(np.random.randint(0, 2**n), f"0{n}b")
        suite.append((build_grover_circuit(n, target), target, f"Grover n={n}"))

        # Deutsch-Jozsa (constant → measures all zeros)
        suite.append((build_dj_circuit(n, "constant"), "0" * n, f"DJ-const n={n}"))

        # Bernstein-Vazirani
        secret = format(np.random.randint(1, 2**n), f"0{n}b")
        suite.append((build_bv_circuit(n, secret), secret, f"BV n={n}"))

    return suite


if __name__ == "__main__":
    print("=== Zero-Noise Extrapolation Benchmark ===\n")
    print("Building circuits...")
    suite = build_suite()
    print(f"  {len(suite)} circuits queued\n")

    print("Running ZNE (5 noise levels × each circuit)...")
    results = run_zne_suite(suite)

    print("\nGenerating plots...")
    plot_noise_scaling_curves(
        results,
        os.path.join(RESULTS_DIR, "zne_scaling_curves.png")
    )
    plot_mitigation_comparison(
        results,
        os.path.join(RESULTS_DIR, "zne_comparison.png")
    )
    plot_mitigation_gain(
        results,
        os.path.join(RESULTS_DIR, "zne_residuals.png")
    )

    # Summary table
    print("\n── Summary ─────────────────────────────────────────────────────")
    print(f"{'Circuit':<18} {'Raw':>6} {'Linear':>8} {'Exp':>8} {'Rich':>8} {'Ideal':>7}")
    print("─" * 65)
    for r in results:
        print(
            f"{r['label']:<18} "
            f"{r['raw_noisy']:>6.3f} "
            f"{r['linear']:>8.3f} "
            f"{r['exponential']:>8.3f} "
            f"{r['richardson']:>8.3f} "
            f"{r['ideal']:>7.3f}"
        )
    print("\n=== Done! Plots saved to ./results/ ===")
