# Refactoring & Modernization Plan

## Phase 0: Baseline & Risk Guardrails
- Instrument frame timing, network latency, and thread utilization across Main.cpp, GuiAppMain.cpp, BinanceRest.cpp, and WebSocket.cpp; capture representative log snapshots for comparison.
- Establish a repeatable smoke test matrix covering GUI launch, order placement, and WebSocket streaming; version the checklist alongside this plan.
- Configure crash dump collection, WinDbg scripts, and rollback runbooks so every subsequent phase maintains functional parity with zero regressions.

## Phase 1: Repository Restructure & Module Boundaries
- Adopt a layered directory layout:
  - `apps/` (console, GUI, background services)
  - `src/core/` (execution engine, data pipelines)
  - `src/net/` (REST, WebSocket clients)
  - `src/ui/` (ImGui views, widgets, theming)
  - `include/` mirrored headers
  - `tests/` for automation scaffolding
  - `assets/` (imgui.ini, themes), `docs/`, `tools/`
- Update `.vcxproj` filters to match the new hierarchy and ensure MSBuild/VS references remain stable.
- Move vendored libraries to `third_party/` with versioned subfolders and add a sync script for upgrades.

## Phase 2: Performance & Multithreading Foundation
- Partition work into dedicated render, network I/O, and data-processing pools using `std::jthread` or a tuned thread pool; exchange data via lock-free ring buffers or bounded concurrent queues.
- Move blocking REST requests onto asynchronous pipelines and keep the GUI message pump single-responsibility; adopt event-driven dispatch for WebSocket callbacks.
- Profile CPU hotspots; evaluate SIMD (AVX2) paths for heavy math and ensure Boost ASIO executors scale with measured workload.

## Phase 3: Server Latency Minimization
- Implement batched signing for REST calls, shared nonce management, and adaptive retry policies encapsulated in a policy class.
- Cache frequently requested metadata (symbols, fees) with TTL refresh; design a lightweight pub/sub to invalidate dependents.
- Emit latency metrics (p95, p99, throughput) via a Prometheus-friendly endpoint and trigger watchdog alerts when SLOs drift.

## Phase 4: Stock Client Feature Expansion
- Portfolio view: add multi-symbol watchlists, aggregated P&L charts, and per-exchange balances with real-time updates.
- Order management: implement advanced order types (OCO, trailing stop), client-side validation, and editable order history.
- Risk controls: include exposure limits, alert thresholds, and sandbox/backtest modes fed by historical data snapshots.
- Scripting/automation: design a lightweight strategy sandbox (Lua or embedded DSL) guarded by permission checks to prevent exploit paths.

## Phase 5: FPS Maximization & Render Loop
- Double-buffer ImGui draw data, using a staging queue for GPU uploads to avoid stalls.
- Separate static from dynamic widgets; reuse cached draw lists for static panels and collapse redundant draw calls.
- Introduce a frame graph that logs update, simulation, and render costs each frame with a 120 FPS target gate.

## Phase 6: Modern UI & Visual Polish
- Define theme tokens (color, spacing, typography) in `assets/style.json` and load at startup; centralize style tweaks.
- Redesign layouts with modular card components, status badges, responsive chart overlays, and contextual tooltips for clarity.
- Add accessibility checks for contrast, keyboard navigation, and HiDPI scaling before feature sign-off.

Every phase must ship bug-free by re-running baseline checks, expanding tests as new features land, and halting rollout if regressions appear.
