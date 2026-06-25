"""
Zero-Noise Extrapolation (ZNE) Error Mitigation
------------------------------------------------
ZNE is a hardware-agnostic error mitigation technique that:
  1. Intentionally scales noise to higher levels via gate folding
  2. Measures the observable (success rate) at each noise level
  3. Fits a model to the noise-vs-expectation curve
  4. Extrapolates back to the zero-noise limit

This is one of the most widely used mitigation strategies on NISQ devices
(used in Mitiq, Qiskit Runtime, and PennyLane).

References:
  - Temme et al., "Error Mitigation for Short-Depth Quantum Circuits" (PRL 2017)
  - Li & Benjamin, "Efficient Variational Quantum Simulator..." (PRX 2017)
  - LaRose et al., "Mitiq: A software package for error mitigation" (Quantum 2022)
"""

import numpy as np
from typing import Callable
from scipy.optimize import curve_fit
from qiskit import QuantumCircuit, transpile
from qiskit_aer import AerSimulator
from qiskit_aer.noise import NoiseModel, depolarizing_error, thermal_relaxation_error


# ── Noise model factory ───────────────────────────────────────────────────────

def build_scaled_noise_model(scale_factor: float) -> NoiseModel:
    """
    Builds a noise model with errors scaled by `scale_factor`.
    scale_factor=1.0 → baseline realistic noise.
    scale_factor=2.0 → 2× noise, etc.

    Uses depolarizing + thermal relaxation on gates.
    """
    p1q = 0.001 * scale_factor
    p2q = 0.010 * scale_factor
    t1   = 50e3
    t2   = min(2 * t1, 70e3 / scale_factor)   # T2 degrades with noise
    gate_time = 50

    nm = NoiseModel()
    tr_1q = thermal_relaxation_error(t1, t2, gate_time)
    tr_2q = tr_1q.expand(thermal_relaxation_error(t1, t2, gate_time))
    dp_1q = depolarizing_error(min(p1q, 0.499), 1)
    dp_2q = depolarizing_error(min(p2q, 0.999), 2)
    err_1q = dp_1q.compose(tr_1q)
    err_2q = dp_2q.compose(tr_2q)

    for gate in ["u1", "u2", "u3", "id", "rz", "sx", "x"]:
        nm.add_all_qubit_quantum_error(err_1q, gate)
    for gate in ["cx", "cz", "ecr"]:
        nm.add_all_qubit_quantum_error(err_2q, gate)
    return nm


# ── Gate folding: noise scaling ───────────────────────────────────────────────

def fold_gates(circuit: QuantumCircuit, scale_factor: float) -> QuantumCircuit:
    """
    Noise amplification via gate folding: G → G G† G (×k times).

    For scale_factor λ, each gate is replaced with:
        G (G† G)^k  where k = (λ - 1) / 2   (integer part)

    Tail gates are added to non-measurement layers to hit fractional scales.
    Only non-measurement, non-barrier gates are folded.

    Args:
        circuit      : original QuantumCircuit (with measurements)
        scale_factor : noise amplification factor (≥ 1.0, typically odd integers)

    Returns:
        New QuantumCircuit with folded gates + original measurements appended
    """
    if scale_factor <= 1.0:
        return circuit.copy()

    # Separate measurements from the main circuit
    n_qubits = circuit.num_qubits
    n_clbits = circuit.num_clbits
    folded = QuantumCircuit(n_qubits, n_clbits)

    # Collect non-measurement instructions
    gate_ops = [
        (instr, qargs, cargs)
        for instr, qargs, cargs in circuit.data
        if instr.name not in ("measure", "barrier", "reset")
    ]
    measure_ops = [
        (instr, qargs, cargs)
        for instr, qargs, cargs in circuit.data
        if instr.name == "measure"
    ]

    # How many full G†G pairs per gate?
    num_pairs = int((scale_factor - 1) / 2)
    # Fraction of gates to fold one extra time (for non-integer scales)
    tail_fraction = (scale_factor - (2 * num_pairs + 1)) / 2
    num_tail = int(round(tail_fraction * len(gate_ops)))

    for idx, (instr, qargs, cargs) in enumerate(gate_ops):
        folded.append(instr, qargs, cargs)
        extra = num_pairs + (1 if idx < num_tail else 0)
        for _ in range(extra):
            try:
                inv = instr.inverse()
                folded.append(inv, qargs, cargs)
                folded.append(instr, qargs, cargs)
            except Exception:
                # Not all gates are invertible (e.g. custom); skip
                pass

    # Re-append measurements
    for instr, qargs, cargs in measure_ops:
        folded.append(instr, qargs, cargs)

    return folded


# ── Extrapolation models ──────────────────────────────────────────────────────

def linear_extrapolate(scale_factors: np.ndarray, expectations: np.ndarray) -> float:
    """
    Fit a line E(λ) = a·λ + b and return E(0) = b.
    Appropriate for shallow circuits with moderate noise.
    """
    coeffs = np.polyfit(scale_factors, expectations, 1)
    return float(coeffs[1])


def exponential_extrapolate(scale_factors: np.ndarray, expectations: np.ndarray) -> float:
    """
    Fit E(λ) = A · exp(-B·λ) + C and return E(0) = A + C.
    More accurate for deeper circuits where noise has exponential effect.
    """
    def exp_model(x, A, B, C):
        return A * np.exp(-B * x) + C

    try:
        p0 = [expectations[0] - expectations[-1], 0.1, expectations[-1]]
        popt, _ = curve_fit(exp_model, scale_factors, expectations, p0=p0, maxfev=5000)
        return float(popt[0] + popt[2])   # A + C = value at λ=0
    except RuntimeError:
        # Fall back to linear if exponential fit fails
        return linear_extrapolate(scale_factors, expectations)


def richardson_extrapolate(scale_factors: np.ndarray, expectations: np.ndarray) -> float:
    """
    Richardson extrapolation: polynomial interpolation through all points,
    evaluated at λ=0. Exact for a polynomial noise model of degree n-1.
    """
    poly = np.polyfit(scale_factors, expectations, deg=len(scale_factors) - 1)
    return float(np.polyval(poly, 0))


# ── ZNE runner ────────────────────────────────────────────────────────────────

SCALE_FACTORS = [1, 2, 3, 4, 5]   # Noise amplification levels
SHOTS         = 4096


def run_zne(
    circuit: QuantumCircuit,
    target: str,
    shots: int = SHOTS,
) -> dict:
    """
    Runs ZNE for a single circuit and returns mitigated estimates.

    Steps:
      1. Run at multiple noise scale factors (gate folding)
      2. Collect success rates at each level
      3. Extrapolate to λ=0 using linear, exponential, and Richardson methods
      4. Compare against ideal (noiseless) simulation

    Args:
        circuit : QuantumCircuit with measurements
        target  : expected measurement bitstring for success rate
        shots   : number of measurement shots per noise level

    Returns:
        dict with keys: scale_factors, expectations, linear, exponential,
                        richardson, ideal
    """
    ideal_sim = AerSimulator()
    expectations = []

    for sf in SCALE_FACTORS:
        folded = fold_gates(circuit, scale_factor=sf)
        nm = build_scaled_noise_model(sf)
        sim = AerSimulator(noise_model=nm)
        t = transpile(folded, sim)
        counts = sim.run(t, shots=shots).result().get_counts()
        sr = counts.get(target, 0) / shots
        expectations.append(sr)

    sf_arr = np.array(SCALE_FACTORS, dtype=float)
    exp_arr = np.array(expectations, dtype=float)

    # Ideal reference
    t_ideal = transpile(circuit, ideal_sim)
    ideal_counts = ideal_sim.run(t_ideal, shots=shots).result().get_counts()
    ideal_sr = ideal_counts.get(target, 0) / shots

    return {
        "scale_factors":  SCALE_FACTORS,
        "expectations":   expectations,
        "linear":         np.clip(linear_extrapolate(sf_arr, exp_arr), 0, 1),
        "exponential":    np.clip(exponential_extrapolate(sf_arr, exp_arr), 0, 1),
        "richardson":     np.clip(richardson_extrapolate(sf_arr, exp_arr), 0, 1),
        "ideal":          ideal_sr,
        "raw_noisy":      expectations[0],   # scale=1 baseline
    }


# ── Batch ZNE across algorithms ───────────────────────────────────────────────

def run_zne_suite(circuits_and_targets: list[tuple[QuantumCircuit, str, str]]) -> list[dict]:
    """
    Runs ZNE for a list of (circuit, target, label) tuples.

    Args:
        circuits_and_targets : list of (circuit, target_bitstring, label)

    Returns:
        List of result dicts, each including the label.
    """
    results = []
    for circuit, target, label in circuits_and_targets:
        print(f"  ZNE: {label} ...", end=" ", flush=True)
        res = run_zne(circuit, target)
        res["label"] = label
        results.append(res)
        improvement = res["exponential"] - res["raw_noisy"]
        print(f"raw={res['raw_noisy']:.3f} → exp={res['exponential']:.3f} "
              f"(+{improvement:+.3f}) ideal={res['ideal']:.3f}")
    return results
