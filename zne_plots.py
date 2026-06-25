"""
ZNE Visualization
-----------------
Generates publication-quality plots for Zero-Noise Extrapolation results.
"""

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.lines import Line2D
import os

RESULTS_DIR = "results"
BG       = "#0D1117"
PANEL_BG = "#161B22"
BORDER   = "#30363D"
TEXT     = "#E6EDF3"
MUTED    = "#8B949E"
GRID_CLR = "#21262D"

ALGO_COLORS = {
    "grover": "#4C72B0",
    "dj":     "#55A868",
    "bv":     "#C44E52",
}

METHOD_STYLES = {
    "Raw (noisy)":    {"color": "#8B949E", "ls": "--",  "marker": "o", "lw": 1.5},
    "Linear ZNE":     {"color": "#DD8452", "ls": "-.",  "marker": "s", "lw": 1.8},
    "Exponential ZNE":{"color": "#4C72B0", "ls": "-",   "marker": "D", "lw": 2.2},
    "Richardson ZNE": {"color": "#55A868", "ls": ":",   "marker": "^", "lw": 1.8},
    "Ideal":          {"color": "#FFD700", "ls": "-",   "marker": "*", "lw": 2.0},
}


def _style_ax(ax):
    ax.set_facecolor(PANEL_BG)
    ax.tick_params(colors=MUTED, labelsize=8)
    ax.spines[:].set_color(BORDER)
    ax.xaxis.label.set_color(MUTED)
    ax.yaxis.label.set_color(MUTED)
    ax.title.set_color(TEXT)
    ax.grid(True, color=GRID_CLR, lw=0.8, alpha=0.7)


def plot_noise_scaling_curves(zne_results: list[dict], out_path: str):
    """
    For each result, plot the expectation value vs noise scale factor,
    showing the fitted extrapolation curves and zero-noise intercepts.
    """
    n = len(zne_results)
    cols = min(3, n)
    rows = (n + cols - 1) // cols

    fig, axes = plt.subplots(rows, cols, figsize=(5.5 * cols, 4.5 * rows))
    fig.patch.set_facecolor(BG)
    axes = np.array(axes).flatten()

    for idx, res in enumerate(zne_results):
        ax = axes[idx]
        _style_ax(ax)

        sfs = np.array(res["scale_factors"], dtype=float)
        exp = np.array(res["expectations"],  dtype=float)

        # Raw data points
        ax.scatter(sfs, exp, color="#8B949E", s=50, zorder=5, label="Noisy samples")

        # Fit curves extended to 0
        x_fit = np.linspace(0, max(sfs), 200)

        # Linear fit
        coeffs = np.polyfit(sfs, exp, 1)
        ax.plot(x_fit, np.polyval(coeffs, x_fit),
                ls="-.", color="#DD8452", lw=1.8, label="Linear fit")

        # Exponential fit
        from scipy.optimize import curve_fit
        def exp_model(x, A, B, C): return A * np.exp(-B * x) + C
        try:
            p0 = [exp[0] - exp[-1], 0.1, exp[-1]]
            popt, _ = curve_fit(exp_model, sfs, exp, p0=p0, maxfev=5000)
            ax.plot(x_fit, exp_model(x_fit, *popt),
                    ls="-", color="#4C72B0", lw=2.2, label="Exponential fit")
        except Exception:
            pass

        # Zero-noise intercepts
        ax.axvline(0, color=BORDER, lw=1, ls=":")
        ax.plot(0, res["linear"],      "s", color="#DD8452", ms=9, zorder=6)
        ax.plot(0, res["exponential"], "D", color="#4C72B0", ms=9, zorder=6)
        ax.plot(0, res["ideal"],       "*", color="#FFD700", ms=12, zorder=7,
                label=f"Ideal = {res['ideal']:.3f}")

        ax.set_title(res["label"], fontsize=10, fontweight="bold")
        ax.set_xlabel("Noise Scale Factor λ")
        ax.set_ylabel("Success Rate")
        ax.set_xlim(-0.3, max(sfs) + 0.3)
        ax.set_ylim(max(0, min(exp) - 0.1), 1.05)
        ax.legend(facecolor="#21262D", edgecolor=BORDER,
                  labelcolor=TEXT, fontsize=7, loc="upper right")

    # Hide unused axes
    for i in range(len(zne_results), len(axes)):
        axes[i].set_visible(False)

    fig.suptitle("ZNE: Noise Scaling Curves & Extrapolation",
                 color=TEXT, fontsize=14, fontweight="bold", y=1.01)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight", facecolor=BG)
    plt.close()
    print(f"  Saved → {out_path}")


def plot_mitigation_comparison(zne_results: list[dict], out_path: str):
    """
    Bar chart comparing raw noisy, three ZNE methods, and ideal
    across all benchmarked circuits.
    """
    labels   = [r["label"] for r in zne_results]
    raw      = [r["raw_noisy"]   for r in zne_results]
    linear   = [r["linear"]      for r in zne_results]
    expon    = [r["exponential"] for r in zne_results]
    richard  = [r["richardson"]  for r in zne_results]
    ideal    = [r["ideal"]       for r in zne_results]

    x   = np.arange(len(labels))
    w   = 0.15
    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 1.6), 6))
    fig.patch.set_facecolor(BG)
    _style_ax(ax)

    ax.bar(x - 2*w, raw,     w, label="Raw (noisy)",     color="#8B949E", edgecolor=BORDER)
    ax.bar(x - 1*w, linear,  w, label="Linear ZNE",      color="#DD8452", edgecolor=BORDER)
    ax.bar(x,        expon,  w, label="Exponential ZNE", color="#4C72B0", edgecolor=BORDER)
    ax.bar(x + 1*w, richard, w, label="Richardson ZNE",  color="#55A868", edgecolor=BORDER)
    ax.bar(x + 2*w, ideal,   w, label="Ideal",           color="#FFD700", edgecolor=BORDER, alpha=0.9)

    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("Success Rate")
    ax.set_ylim(0, 1.12)
    ax.set_title("Mitigation Comparison: Raw vs ZNE Methods vs Ideal",
                 color=TEXT, fontsize=13, fontweight="bold")
    ax.legend(facecolor="#21262D", edgecolor=BORDER, labelcolor=TEXT, fontsize=9)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight", facecolor=BG)
    plt.close()
    print(f"  Saved → {out_path}")


def plot_mitigation_gain(zne_results: list[dict], out_path: str):
    """
    Scatter plot of residual error (method − ideal) for each circuit.
    Points closer to y=0 are more accurate. Shows ZNE closing the gap.
    """
    fig, ax = plt.subplots(figsize=(10, 5))
    fig.patch.set_facecolor(BG)
    _style_ax(ax)

    labels = [r["label"] for r in zne_results]
    x = np.arange(len(labels))

    for method, key, style in [
        ("Raw (noisy)",     "raw_noisy",   METHOD_STYLES["Raw (noisy)"]),
        ("Linear ZNE",      "linear",      METHOD_STYLES["Linear ZNE"]),
        ("Exponential ZNE", "exponential", METHOD_STYLES["Exponential ZNE"]),
        ("Richardson ZNE",  "richardson",  METHOD_STYLES["Richardson ZNE"]),
    ]:
        residuals = [abs(r[key] - r["ideal"]) for r in zne_results]
        ax.plot(x, residuals, marker=style["marker"], ls=style["ls"],
                color=style["color"], lw=style["lw"], label=method, ms=7)

    ax.axhline(0, color="#FFD700", lw=1.2, ls="--", label="Ideal (residual=0)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("|Estimated − Ideal|")
    ax.set_title("Residual Error vs Ideal — Lower is Better",
                 color=TEXT, fontsize=13, fontweight="bold")
    ax.legend(facecolor="#21262D", edgecolor=BORDER, labelcolor=TEXT, fontsize=9)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight", facecolor=BG)
    plt.close()
    print(f"  Saved → {out_path}")
