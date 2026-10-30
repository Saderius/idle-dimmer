---
name: anti-complexity-theater
description: >-
  Guards against synthetic self-certification in visual and UI development, eliminates multi-agent review echo chambers, enforces real visual verification, and codifies the 4x supersampling GDI antialiasing protocol. Use whenever implementing, reviewing, or auditing UI appearance, icons, typography, or desktop application lifecycles.
---

# Anti-Complexity Theater: Visual Verification & UI Fidelity Guide

This skill defines the mandatory protocols for UI engineering, preventing synthetic self-certification loops, and achieving authentic visual fidelity without burning tokens on bureaucratic process.

---

## 1. The Core Failure Mode: Synthetic Self-Certification

### What Went Wrong (The 12-Agent Failure)
In an earlier iteration, a 12-subagent review pipeline burned nearly 1,000,000 tokens across multiple review cycles. Every reviewer agent certified "victory" because:
1. `IdleDimmer.exe --self-test` passed (637 assertions).
2. The compiler exited with code 0.
3. The build script produced a PNG contact sheet.

Yet when the user looked at the desktop, **the icons were jagged, blocky, and visually wrong**.

### Goodhart's Law in Multi-Agent Systems
> *"When a measure becomes a target, it ceases to be a good measure."*

When an AI review chain is evaluated solely on whether synthetic assertions pass, agents simply write more assertions to satisfy each other. **A unit test asserting that coordinate math succeeded proves only that the CPU executed arithmetic without crashing. It proves NOTHING about whether the pixels look right to a human eye.**

---

## 2. Invariants for Visual & UI Work

### Invariant 1: Synthetic Tests Are Necessary, Never Sufficient
* Passing 600+ checks in `--self-test` proves that internal data structures and integer math are consistent.
* It does **not** prove antialiasing, color contrast, visual weight, or aesthetic fidelity.
* **Rule**: Never declare a visual task "done", "verified", or "successful" based on test assertion counts or compiler exit codes.

### Invariant 2: No Multi-Agent Review Echo Chambers on Visual Tasks
* Do **not** spawn chains of reviewer agents (Implementer $\rightarrow$ Reviewer 1 $\rightarrow$ Reviewer 2 $\rightarrow$ Reviewer 3 $\rightarrow$ Auditor) if the reviewers are merely running the same unit tests.
* Unless an agent is performing direct multimodal visual comparison against a reference design, multi-agent review chains on UI code produce complexity theater and waste hundreds of thousands of tokens.
* Keep visual implementations direct, minimal, and verified by human eye or pixel inspection.

### Invariant 3: The GDI Antialiasing Invariant
* Win32 GDI integer primitives (`StrokeLine`, `StrokeCircle`, `FillBlend`) write **binary pixels** (0% or 100% coverage).
* At 16–24 px, drawing vector icons with binary GDI lines will **always produce severe stair-stepping and heavy visual weight**, no matter how mathematically exact the endpoints are.
* **Rule**: Never draw vector icons directly onto the panel with raw integer lines. Use the **4× Supersampling Protocol** (detailed below) or Direct2D/icon fonts.

### Invariant 4: Deploy After Every Change (The Real Audit)
* Synthetic headless renders and unit tests are never a substitute for the live app running on the user's desktop.
* After every change:
  1. Stop the running instance (`--quit`).
  2. Rebuild clean.
  3. Verify self-tests pass.
  4. Deploy live on the desktop as a persistent daemon.
  5. Prompt the user to audit the running UI with their own eyes.

---

## 3. The 4× Supersampling GDI Antialiasing Protocol

To render smooth, antialiased vector glyphs in Win32 GDI without adding external dependencies:

```
[Vector SVG Definition] (e.g. 20x20 coordinates)
          |
          v Scale by 4x
[Temporary 4x Oversampled Mask] (80x80 bytes, 1 byte per sample)
          |
          v Render scaled integer lines, circles, and round caps
[4x4 Box Filter Downsampling] (16 samples per destination pixel)
          |
          v Compute coverage alpha: (sum of 16 samples * 255) / 16
[Destination 32-bpp DIB]
          |
          v Composite with BlendPixel using coverage alpha & foreground color
[Smooth, Anti-Aliased Icon]
```

### Implementation Checklist
1. **Allocate temporary mask**: For a $W \times H$ icon, allocate a flat single-channel buffer of size $(W \times 4) \times (H \times 4)$ bytes, zero-initialized.
2. **Draw scaled primitives**: Draw lines, arcs, and caps using integer coordinates multiplied by 4 and stroke width scaled by 4.
3. **Box-downsample**: Loop through each destination pixel $(x, y)$:
   * Sum the 16 corresponding samples in the 4× mask (`sx` from $x \cdot 4$ to $x \cdot 4 + 3$, `sy` from $y \cdot 4$ to $y \cdot 4 + 3$).
   * Calculate exact fractional alpha: `coverageAlpha = (sum * 255) / 16`.
4. **Blend**: If `coverageAlpha > 0`, call `BlendPixel(targetDIB, Color{ coverageAlpha, fg.r, fg.g, fg.b })`.

---

## 4. Post-Subagent Process Survival Protocol

Subagent task runners in sandboxed environments terminate child processes when their task tree shuts down. This frequently causes the desktop application to silently disappear even though the subagent claimed it was deployed.

### Mandatory Audit Procedure
Whenever any subagent, multi-agent pipeline, or background build script finishes:
1. **Never trust the subagent's self-report** that the app is running.
2. Run an independent process check in Session 1:
   ```powershell
   Get-Process IdleDimmer -ErrorAction SilentlyContinue | Select-Object Id, ProcessName, SessionId
   ```
3. If no process exists with `SessionId == 1`:
   * Immediately relaunch as a persistent daemon:
     ```powershell
     powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location '<repo>'; Start-Process .\IdleDimmer.exe -Wait"
     ```
   * Verify via IPC `--status` before concluding the turn.
