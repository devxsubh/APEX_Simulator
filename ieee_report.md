\documentclass[conference]{IEEEtran}
\IEEEoverridecommandlockouts
\usepackage{cite}
\usepackage{amsmath,amssymb,amsfonts}
\usepackage{graphicx}
\usepackage{textcomp}
\usepackage{xcolor}
\usepackage{booktabs}
\newtheorem{theorem}{Theorem}
\begin{document}

\title{APEX-CFS: Approximate Decay and Cube-Root Arithmetic in a CFS-Style Scheduler Model, with Analytic Error Bounds and Measured Costs}

\author{\IEEEauthorblockN{Divyanshi Sachan}
\IEEEauthorblockA{\textit{Rishihood University}\\
divyanshi.s23csai@nst.rishihood.edu.in}
\and
\IEEEauthorblockN{Subham Mahapatra}
\IEEEauthorblockA{\textit{Rishihood University}\\
subham.m23csai@nst.rishihood.edu.in}
\and
\IEEEauthorblockN{Manasa Chinnam}
\IEEEauthorblockA{\textit{Rishihood University}\\
manasa.c23csai@nst.rishihood.edu.in}
}

\maketitle

\begin{abstract}
APEX-CFS is a userspace simulator that replaces the exponential load-decay factor of a CFS-style scheduler model with cheaper approximations (first-order, second-order polynomial, and a 256-entry lookup table), plus an adaptive controller that switches between them. It also studies dropping the Newton step from a table-based cube root of the kind used in TCP CUBIC. We prove a cumulative error bound for exponentially weighted load tracking (Theorem~2) and check it, together with analytic per-step bounds, against a shadow exact tracker on two workloads: all 10 checks hold, the per-step bounds are tight (94\%--97\% reached) and the cumulative bound is loose (0.03\%--4.67\% reached). Scheduling impact is measured by pairing each mode against an exact run on the same workload. At the default load coupling of 0.02, POLY2 and ADAPTIVE make the same decisions as EXACT, LINEAR differs on 0.90\% of ticks and LUT256 on 14.05\%; these figures change substantially with the coupling constant, which is a modeling assumption with no counterpart in Linux. On one arm64 machine, exact \texttt{exp()} costs 1.77~ns net per call and POLY2 0.29~ns (median of 11 runs); LINEAR and LUT256 are below what our benchmark resolves, and dropping the Newton step from the cube root saves 0.29~ns per call. We list the limitations that keep these results from transferring directly to the kernel.
\end{abstract}

\begin{IEEEkeywords}
approximate computing, Linux CFS, PELT, load tracking, error bounds, TCP CUBIC, microbenchmarking
\end{IEEEkeywords}

\section{Introduction}
Linux CFS schedules by weighted virtual runtime, and per-entity load tracking (PELT) maintains exponentially decaying load averages \cite{cfsdesign,peltlwn}. Approximate computing trades numerical accuracy for cost in many systems \cite{meantime,pliant,agrawal}. This project asks a narrow question in a userspace model: if the decay factor $e^{-\lambda t}$ is replaced by a cheaper approximation, how large is the resulting load error, can it be bounded analytically, how often does it change a scheduling decision, and what does it save?

\textbf{Relation to the earlier version.} An earlier version of this report and its notebook presented results that were partly produced by hardcoded constants (a fixed cycle count for the CUBIC routine), by a branch that applied only to the 11-task workload of Experiment~2 and scaled the decay error into task weights, and by an error check at a single point against bounds chosen after the fact. Those code paths have been removed and their results are withdrawn. Every number below comes from the current simulator.

Contributions:
\begin{itemize}
    \item A single simulator codebase with five decay modes (EXACT, LINEAR, LUT256, POLY2, ADAPTIVE) and an adaptive controller with state telemetry.
    \item Theorem~2, a cumulative bound on load-tracking error under approximate decay, with proof, and analytic per-step bounds for each mode.
    \item A shadow exact load tracker inside the simulation, so observed load error is pure arithmetic error, checked against those bounds over whole runs on two workloads.
    \item A paired-vs-EXACT metric for scheduling impact (share of ticks whose decision differs, first divergence, final vruntime gap), and a sensitivity study of the load-coupling assumption it depends on.
    \item Measured per-call costs with a call-only baseline subtracted, repeated across runs to expose noise.
\end{itemize}

\section{Model}
\subsection{Scheduler loop}
Each tick of 32~ms ($\Delta t$) the simulator: (1) updates every task's load average with the mode's decay factor $\tilde d$ over a window $t = \Delta t\,(1 + 0.05\,w)$, where $w$ is the number of ticks since the task last ran; (2) updates a shadow load average $L$ with the exact factor $d = e^{-\lambda t}$, $\lambda = 0.693$, on the same input; (3) sets effective weight; (4) runs the runnable task with minimum vruntime and advances its vruntime by $32 \cdot 1024 / w_{\mathrm{eff}}$. The load input is $c = 1/(1+w)$ for a runnable task and 0 otherwise, so $c \in [0,1]$. A task counts a starvation event for every tick it waits beyond $2\max(n, 20)$ ticks.

\subsection{Load coupling (modeling assumption)}
Effective weight is
\begin{equation}
w_{\mathrm{eff}} = w \cdot \max\bigl(0.25,\ 1 + \kappa\,(\tilde L - 0.5)\bigr), \qquad \kappa = 0.02.
\end{equation}
This is the only path by which decay approximation reaches scheduling decisions. Real CFS does not feed PELT into vruntime; PELT matters for load balancing and frequency selection (schedutil, EAS). Experiment~2b varies $\kappa$.

\subsection{Approximation modes}
With $x = \lambda t$:
\begin{itemize}
    \item EXACT: $e^{-x}$.
    \item LINEAR: $\max(0, 1 - x)$.
    \item LUT256: a 256-entry table of $e^{-\lambda i \Delta t}$ rounded to four decimals, indexed by $\mathrm{round}(t/\Delta t)$.
    \item POLY2: $1 - x + x^2/2$, clamped to $[0,1]$.
    \item ADAPTIVE: POLY2 in state SAFE, LUT256 in CAUTION, EXACT in STRICT.
\end{itemize}

\subsection{Adaptive controller}
Every 8 ticks (256~ms) the controller compares a fairness signal $f$ with thresholds scaled by the minimum task weight $w_{\min}$: SAFE$\to$CAUTION if $f > 0.05\,w_{\min}$, CAUTION$\to$STRICT if $f > 0.10\,w_{\min}$, CAUTION$\to$SAFE if $f < 0.02\,w_{\min}$, STRICT$\to$CAUTION if $f < 0.04\,w_{\min}$. The signal is the maximum pairwise vruntime gap; in the phased Experiment~3 it is that gap divided by the mean runnable vruntime, in percent. Section~\ref{sec:limits} discusses the mismatch of units.

\section{Error Bounds}
\subsection{Per-step bounds}
For $t \in [0, t_{\max}]$ and $x_{\max} = \lambda t_{\max}$:
\begin{itemize}
    \item LINEAR: $0 \le e^{-x} - (1-x) \le x^2/2$ (Taylor remainder), so $\delta = x_{\max}^2/2$.
    \item POLY2: the alternating Taylor remainder gives $|e^{-x} - (1-x+x^2/2)| \le x^3/6$, so $\delta = x_{\max}^3/6$. Clamping to $[0,1]$ only moves the value toward $e^{-x}$.
    \item LUT256: index rounding moves $t$ by at most $\Delta t/2$ and $e^{-\lambda t}$ has Lipschitz constant $\lambda$, so the error is at most $\lambda \Delta t/2$ plus the table quantization ($5.1\times 10^{-5}$, covering four-decimal rounding and single-precision conversion). For $\Delta t = 32$~ms this is $1.114\times 10^{-2}$, independent of $t_{\max}$ within the table range.
    \item ADAPTIVE: the larger of the POLY2 and LUT256 bounds.
\end{itemize}
The formulas are fixed in the code before a run; each is evaluated at the largest decay window $t_{\max}$ the run actually used.

\subsection{Cumulative bound}
\setcounter{theorem}{1}
\begin{theorem}[Cumulative load error]
Let $L_{k+1} = d_k L_k + (1-d_k)\,c_k$ and $\tilde L_{k+1} = \tilde d_k \tilde L_k + (1-\tilde d_k)\,c_k$ with $c_k \in [0,1]$, $d_k \in [0,1]$ and $L_0 = \tilde L_0 \in [0,1]$. If $|\tilde d_k - d_k| \le \delta$ and $0 \le \tilde d_k \le D < 1$ for all $k$, then for all $k$
\begin{equation}
|\tilde L_k - L_k| \le \delta\,\frac{1 - D^k}{1 - D} \le \frac{\delta}{1-D}.
\end{equation}
\end{theorem}
\begin{IEEEproof}
Let $e_k = \tilde L_k - L_k$. Subtracting the updates,
\begin{equation}
e_{k+1} = \tilde d_k e_k + (\tilde d_k - d_k)(L_k - c_k).
\end{equation}
$L_k$ is a convex combination of $L_0$ and earlier inputs, all in $[0,1]$, so $L_k \in [0,1]$ and $|L_k - c_k| \le 1$. Hence $|e_{k+1}| \le D\,|e_k| + \delta$. With $e_0 = 0$, induction gives $|e_k| \le \delta \sum_{j=0}^{k-1} D^j = \delta (1-D^k)/(1-D)$.
\end{IEEEproof}

In the simulator, $\delta$ is the per-step bound above and $D$ is the largest value the approximate decay takes on $[\Delta t, t_{\max}]$. Each variant is non-increasing in $t$ (LINEAR, LUT256, EXACT) or convex on the interval (POLY2), so $D$ is attained at an endpoint. The theorem is numbered 2 to match the simulator output; the threshold check that an earlier version called Theorem~1 was not a theorem and has been dropped.

\section{Experimental Setup}
Build with \texttt{make all} (GCC driver, \texttt{-O2 -Wall -Wextra}); run \texttt{./apex\_sim --experiment all}. The reference run was made on an arm64 laptop (Darwin 27.0.0, Apple clang 21.0.0 invoked as \texttt{gcc}) from branch \texttt{research/integrity-fixes}. The analysis notebook \texttt{apex\_complete\_analysis.ipynb} performs the build and run, saves the raw output, and exports all parsed tables, run metadata and timing repeats to \texttt{apex\_notebook\_outputs/apex\_results.json}. Workloads are synthetic and deterministic: repeated runs gave identical output for Experiments~1--4. Only the timings of Experiments~5 and~6 vary, so these were run 11 times in total (the reference run plus 10 extra runs) and we report medians and ranges.

\begin{itemize}
    \item Exp 1: $N = 10, 50, 100$ nice-0 tasks, 1000 ticks.
    \item Exp 2: one nice $-10$ task (weight 9548) and ten nice-0 tasks (weight 1024), 2000 ticks, each mode paired with an EXACT run.
    \item Exp 2b: Exp 2 at $\kappa = 0.02, 0.2, 1.0$.
    \item Exp 3: ADAPTIVE, 60 nice-0 tasks, 2000 ticks; runnable count 5 (ticks 0--499), 55 (500--1499), 15 (1500--1999); vruntimes and load averages reset at each boundary.
    \item Exp 4: \texttt{mixed20} (20 tasks, nice $-10$..0) and \texttt{equal100} (100 nice-0 tasks), 1000 ticks each.
    \item Exp 5: 1024 inputs evenly covering $[16384, 20480)$; table-based cube root with one Newton step (LUT+NR) vs table only.
    \item Exp 6: the four decay functions on 1000 windows from 32 to 190~ms.
\end{itemize}
Timings: each function is called through a volatile function pointer over the input array, 2000 repetitions per trial, median of 21 trials after one warm-up. A call-only function (\texttt{return x}) is timed the same way and subtracted to give \emph{net} ns per call.

\section{Results}
\subsection{Experiment 1: Equal-weight fairness}
Table~\ref{tab:exp1} shows that with equal weights all modes behave almost identically: vruntime standard deviation stays below 0.004\% of the mean, the largest pairwise vruntime gap at any tick differs across modes by 0.0003 vruntime units (one nice-0 tick is 32), and there is no starvation.

\begin{table}[htbp]
\caption{Experiment 1: equal-weight fairness}
\label{tab:exp1}
\centering
\resizebox{\linewidth}{!}{%
\begin{tabular}{lrrrrr}
\toprule
Mode & \multicolumn{3}{c}{vdev\% at $N=$} & Max gap & Starv. \\
 & 10 & 50 & 100 & & \\
\midrule
EXACT    & 0.0014 & 0.0025 & 0.0038 & 32.3187 & 0 \\
LINEAR   & 0.0014 & 0.0025 & 0.0038 & 32.3190 & 0 \\
LUT256   & 0.0019 & 0.0025 & 0.0036 & 32.3189 & 0 \\
POLY2    & 0.0014 & 0.0025 & 0.0038 & 32.3187 & 0 \\
ADAPTIVE & 0.0014 & 0.0025 & 0.0038 & 32.3187 & 0 \\
\bottomrule
\end{tabular}}
\end{table}

\subsection{Experiment 2: Mixed weights, paired against EXACT}
CPU share is not a useful separator: the high-priority task receives 48.40\% in every mode except LUT256 (48.45\%), against 48.25\% from weights alone; maximum wait is 27 ticks and there is no starvation in any mode. We therefore compare each mode with an EXACT run on the same workload, tick by tick (Table~\ref{tab:exp2}, Fig.~\ref{fig:exp2}). The last column is the largest per-task vruntime difference from the EXACT run at the end of the run, in nice-0 slices.

\begin{table}[htbp]
\caption{Experiment 2: paired comparison with EXACT ($\kappa = 0.02$)}
\label{tab:exp2}
\centering
\resizebox{\linewidth}{!}{%
\begin{tabular}{lrrr}
\toprule
Mode & Decisions differ & First divergence & End gap \\
 & (\% of ticks) & (tick) & (slices) \\
\midrule
EXACT    & 0.00  & never & 0.0000 \\
LINEAR   & 0.90  & 349   & 0.0013 \\
LUT256   & 14.05 & 349   & 0.9925 \\
POLY2    & 0.00  & never & 0.0000 \\
ADAPTIVE & 0.00  & never & 0.0000 \\
\bottomrule
\end{tabular}}
\end{table}

\begin{figure}[htbp]
\centerline{\includegraphics[width=\linewidth]{apex_notebook_outputs/exp2_decisions_differ.png}}
\caption{Experiment 2: share of ticks whose scheduling decision differs from a paired EXACT run.}
\label{fig:exp2}
\end{figure}

POLY2 and ADAPTIVE never change a decision. LUT256 changes far more decisions than LINEAR, consistent with its larger per-step error on this workload's window range (Section~\ref{sec:exp4}).

\subsection{Experiment 2b: Sensitivity to the load coupling}
Since decay error reaches scheduling only through $\kappa$, we repeat Experiment~2 at three values (Table~\ref{tab:exp2b}, Fig.~\ref{fig:exp2b}). The fraction of differing decisions grows by more than an order of magnitude for LINEAR, and the ordering of modes changes: at $\kappa = 1$ ADAPTIVE differs on 43.35\% of ticks while POLY2 differs on 2.60\%. The simulator does not report controller state occupancy in this experiment, so we do not attribute the ADAPTIVE result further. The conclusion we draw is that Experiment~2 characterizes this model at $\kappa = 0.02$, not Linux.

\begin{table}[htbp]
\caption{Experiment 2b: decisions differing from EXACT (\% of ticks)}
\label{tab:exp2b}
\centering
\resizebox{\linewidth}{!}{%
\begin{tabular}{lrrrr}
\toprule
$\kappa$ & LINEAR & LUT256 & POLY2 & ADAPTIVE \\
\midrule
0.02 & 0.90  & 14.05 & 0.00 & 0.00 \\
0.20 & 28.75 & 63.85 & 0.00 & 0.00 \\
1.00 & 39.00 & 47.45 & 2.60 & 43.35 \\
\bottomrule
\end{tabular}}
\end{table}

\begin{figure}[htbp]
\centerline{\includegraphics[width=\linewidth]{apex_notebook_outputs/exp2b_coupling_sensitivity.png}}
\caption{Experiment 2b: sensitivity of the Experiment 2 result to the load-coupling constant (log scale).}
\label{fig:exp2b}
\end{figure}

\subsection{Experiment 3: Adaptive controller}
Table~\ref{tab:exp3} and Fig.~\ref{fig:exp3} give the controller's state occupancy per phase. The normalized gap is the maximum pairwise vruntime gap divided by the mean runnable vruntime, averaged over the phase. After the spike at tick 500 the controller changed state within 4 ticks (128~ms), inside one 8-tick (256~ms) monitor interval. Transitions over the run: SAFE$\to$CAUTION 2, CAUTION$\to$STRICT 1, STRICT$\to$CAUTION 1, CAUTION$\to$SAFE 1.

\begin{table}[htbp]
\caption{Experiment 3: controller state occupancy (ticks)}
\label{tab:exp3}
\centering
\resizebox{\linewidth}{!}{%
\begin{tabular}{rrrrrr}
\toprule
Phase & Tasks & SAFE & CAUTION & STRICT & Mean norm. gap \\
\midrule
1 & 5  & 484 & 16 & 0   & 3.36\% \\
2 & 55 & 4   & 8  & 988 & 555.32\% \\
3 & 15 & 0   & 16 & 484 & 92.55\% \\
\bottomrule
\end{tabular}}
\end{table}

\begin{figure}[htbp]
\centerline{\includegraphics[width=\linewidth]{apex_notebook_outputs/exp3_state_occupancy.png}}
\caption{Experiment 3: ADAPTIVE state occupancy by phase; each state is colored like the mode it selects.}
\label{fig:exp3}
\end{figure}

The controller uses exact arithmetic for 988 of 1000 ticks in the heaviest phase. Its signal measures contention, not approximation error, so it falls back to the most expensive mode exactly when the most tasks are runnable. This is the opposite of what a cost-saving controller should do, and we do not present it as a working design.

\subsection{Experiment 4: Error bound verification}
\label{sec:exp4}
Table~\ref{tab:exp4} and Fig.~\ref{fig:exp4} compare observed errors with the analytic bounds. All 10 checks hold. For the fixed modes the per-step bounds are tight (observed is 94\%--97\% of the bound); the cumulative bound of Theorem~2 is loose (0.03\%--4.67\%), because it assumes the worst-case per-step error at every step with $|L - c| = 1$.

On \texttt{mixed20} ($t_{\max} = 169.6$~ms), LUT256 has the largest per-step error ($1.068\times 10^{-2}$), not the smallest: rounding a fractional window to the nearest table index costs up to $\lambda\Delta t/2$. On \texttt{equal100}, where windows reach 286.4~ms, LINEAR is largest ($1.846\times 10^{-2}$), since its error grows quadratically with the window. ADAPTIVE's observed errors equal POLY2's on both workloads, consistent with its controller staying in SAFE throughout; its bound is the LUT256 bound because the bound must cover every state it could enter.

\begin{table*}[htbp]
\caption{Experiment 4: analytic bounds vs shadow exact tracker (absolute errors)}
\label{tab:exp4}
\centering
\resizebox{\textwidth}{!}{%
\begin{tabular}{llrrrrrrc}
\toprule
Workload & Mode & $t_{\max}$ (ms) & Step bound $\delta$ & Step observed & Load bound $\delta/(1-D)$ & Load observed & Observed/bound & Verified \\
\midrule
mixed20  & EXACT    & 169.6 & 0         & 0         & 0         & 0         & -     & YES \\
mixed20  & LINEAR   & 169.6 & 6.907e-03 & 6.644e-03 & 3.115e-01 & 1.514e-03 & 0.005 & YES \\
mixed20  & LUT256   & 169.6 & 1.114e-02 & 1.068e-02 & 5.086e-01 & 2.376e-02 & 0.047 & YES \\
mixed20  & POLY2    & 169.6 & 2.706e-04 & 2.628e-04 & 1.234e-02 & 1.728e-05 & 0.001 & YES \\
mixed20  & ADAPTIVE & 169.6 & 1.114e-02 & 2.628e-04 & 5.086e-01 & 1.728e-05 & 0.000 & YES \\
equal100 & EXACT    & 289.6 & 0         & 0         & 0         & 0         & -     & YES \\
equal100 & LINEAR   & 286.4 & 1.970e-02 & 1.846e-02 & 8.882e-01 & 1.007e-03 & 0.001 & YES \\
equal100 & LUT256   & 288.0 & 1.114e-02 & 1.068e-02 & 5.086e-01 & 6.473e-03 & 0.013 & YES \\
equal100 & POLY2    & 289.6 & 1.347e-03 & 1.282e-03 & 6.143e-02 & 1.649e-05 & 0.000 & YES \\
equal100 & ADAPTIVE & 289.6 & 1.114e-02 & 1.282e-03 & 5.086e-01 & 1.649e-05 & 0.000 & YES \\
\bottomrule
\end{tabular}}
\end{table*}

\begin{figure}[htbp]
\centerline{\includegraphics[width=\linewidth]{apex_notebook_outputs/exp4_bounds_vs_observed.png}}
\caption{Experiment 4: observed error (filled) vs analytic bound (hollow), per step and cumulative, log scale. EXACT omitted (both zero).}
\label{fig:exp4}
\end{figure}

\subsection{Experiments 5 and 6: Measured cost}
Fig.~\ref{fig:cost} and Table~\ref{tab:cost} report net ns per call. The call-only baseline was 0.87~ns in the reference run for both experiments.

\begin{table}[htbp]
\caption{Net ns per call, median and range over 11 runs (arm64, \texttt{-O2})}
\label{tab:cost}
\centering
\resizebox{\linewidth}{!}{%
\begin{tabular}{lrr}
\toprule
Function & Median & Range \\
\midrule
EXACT (\texttt{exp})     & 1.77 & 1.76 -- 1.88 \\
LINEAR                   & 0.00 & $-0.01$ -- 0.09 \\
LUT256                   & 0.02 & 0.01 -- 0.03 \\
POLY2                    & 0.29 & 0.28 -- 0.29 \\
\midrule
Cube root, LUT+NR        & 0.30 & 0.29 -- 0.31 \\
Cube root, LUT only      & 0.01 & $-0.02$ -- 0.09 \\
\bottomrule
\end{tabular}}
\end{table}

\begin{figure}[htbp]
\centerline{\includegraphics[width=\linewidth]{apex_notebook_outputs/exp5_6_net_ns.png}}
\caption{Experiments 5 and 6: median net ns per call with min--max whiskers over 11 runs.}
\label{fig:cost}
\end{figure}

\textbf{Decay (Exp 6).} POLY2 is cheaper than \texttt{exp()} by a factor of 6.10 to 6.71 across the 11 runs (EXACT net / POLY2 net per run); the reference run printed 6.18$\times$. LINEAR and LUT256 cost less than a function call, which is below what this method resolves, so no speedup is reported for them. Their maximum absolute errors on the benchmark's windows were $8.334\times 10^{-3}$ (LINEAR), $1.068\times 10^{-2}$ (LUT256) and $3.706\times 10^{-4}$ (POLY2).

\textbf{Cube root (Exp 5).} LUT+NR is itself an approximation of \texttt{cbrt()}: mean relative error 0.000045\%, maximum 0.000159\%. Dropping the Newton step saves 0.29~ns per call (median; 0.22 to 0.31 across runs) at a mean relative error of 0.0582\% (maximum 0.1260\%) against LUT+NR. Because the LUT-only cost is below resolution, no speedup ratio is reported. In Linux, \texttt{cubic\_root()} runs about once per congestion epoch, not per ACK \cite{rfc9438}, so this saving barely reaches throughput.

\textbf{Caveats.} This is a throughput-style benchmark through a function pointer on one machine. It does not measure latency or use cycle counters, has not been run on x86, and compares floating-point routines, while Linux PELT uses an integer table and the kernel's \texttt{cubic\_root()} differs from ours. The simulator prints a speedup whenever a net cost exceeds 0.05~ns, which is inside the run-to-run spread above; a speedup printed for LINEAR or LUT256 in a single run is noise.

\section{Limitations}
\label{sec:limits}
\begin{itemize}
    \item \textbf{Load coupling is invented.} Effective weight is $w(1 + 0.02(\tilde L - 0.5))$. Real CFS does not feed PELT into vruntime; PELT matters for load balancing and frequency selection (schedutil, EAS). Experiment~2b shows the conclusions depend heavily on this constant.
    \item \textbf{Controller signal and units.} Thresholds scaled by $w_{\min}$ are compared against vruntime gaps or, in Experiment~3, a percentage. The signal reflects contention, not approximation error, so ADAPTIVE goes to STRICT exactly when the load is heaviest.
    \item \textbf{Floating point, not kernel fixed point.} Linux PELT already uses an integer table (\texttt{runnable\_avg\_yN\_inv}, $y^{32} = 1/2$, 1024~$\mu$s periods) \cite{linuxsrc}. The CUBIC routine here is not the kernel's \texttt{cubic\_root()}, which runs about once per congestion epoch.
    \item \textbf{Throughput-style microbenchmark} through a function pointer. Functions cheaper than a call are reported as \texttt{n/a}. It does not yet measure latency, use cycle counters, or run on x86.
    \item \textbf{Synthetic, deterministic workloads.} There is no randomness, so there are no seeds or confidence intervals yet; the timing ranges above reflect measurement noise only.
\end{itemize}

\section{Conclusion and Future Work}
In this model, approximating the load-decay factor produces load errors that stay within analytic bounds; the per-step bounds are tight and the cumulative bound of Theorem~2 is conservative. Whether the error changes scheduling decisions depends mainly on an invented coupling constant: at $\kappa = 0.02$ POLY2 matches EXACT decision for decision while saving about 1.5~ns per call over floating-point \texttt{exp()}. LUT256 is cheap but has the largest per-step error on short windows. The adaptive controller, as built, reacts to contention rather than to error and is not yet useful.

Future work: compare against kernel-faithful baselines (integer PELT \texttt{decay\_load()} and the real \texttt{cubic\_root()}) and approximations cheaper than \emph{those}; measure latency as well as throughput with cycle counters on x86 and arm64, together with how often the kernel calls these paths; use randomized workloads with seeds and confidence intervals, and real \texttt{perf sched} traces; follow the error to where PELT matters (schedutil frequency selection and energy); drive the controller from an error estimate, for example periodic shadow-exact sampling; and prototype an approximate BPF CUBIC under \texttt{netem} and a \texttt{sched\_ext} scheduler with its own load tracking.

\section*{Acknowledgment}
This work is a course research prototype built on publicly available Linux scheduling and congestion-control references.

\begin{thebibliography}{00}
\bibitem{cfsdesign} Linux Kernel Documentation, ``CFS Scheduler Design.'' [Online]. Available: https://docs.kernel.org/scheduler/sched-design-CFS.html
\bibitem{peltlwn} J. Corbet, ``Per-entity load tracking,'' LWN.net, 2013. [Online]. Available: https://lwn.net/Articles/531853/
\bibitem{meantime} A. Farrell and H. Hoffmann, ``MEANTIME: Achieving Both Minimal Energy and Timeliness with Approximate Computing,'' in \textit{Proc. USENIX ATC}, 2016.
\bibitem{pliant} N. Kulkarni et al., ``Pliant: Leveraging Approximation to Improve Datacenter Resource Efficiency,'' in \textit{Proc. IEEE HPCA}, 2019.
\bibitem{agrawal} A. Agrawal et al., ``Approximate Computing: Challenges and Opportunities,'' in \textit{Proc. IEEE ICRC}, 2016.
\bibitem{rfc9438} L. Xu, S. Ha, I. Rhee, V. Goel, and L. Eggert, ``CUBIC for Fast and Long-Distance Networks,'' RFC 9438, IETF, 2023.
\bibitem{linuxsrc} Linux Kernel Source Tree, \texttt{kernel/sched/pelt.c} and \texttt{net/ipv4/tcp\_cubic.c}. [Online]. Available: https://github.com/torvalds/linux
\end{thebibliography}

\end{document}
