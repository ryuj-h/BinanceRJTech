# Repository Guidelines

This guide explains how to build, run, and contribute to BinanceRJTech (C++/MSBuild/Visual Studio).

## Project Structure & Module Organization
- Source: C++ entry points and modules live in the repo root (e.g., `Main.cpp`, `GuiAppMain.cpp`, `AppMain.cpp`, `BinanceRest.cpp`, `WebSocket.cpp`).
- Headers: in root and `include/` (e.g., `BinanceRest.hpp`, `WebSocket.hpp`).
- Dependencies: `boost_1_89_0/` and `third_party/` are vendored.
- Artifacts: `x64/` (build outputs), `build/` (optional local builds), `.vs/` (IDE state).
- Config/assets: `.vscode/`, `imgui.ini`, `imgui_layout.ini`, `cert/` (TLS material; do not commit secrets).

## Build, Test, and Development Commands
- Build (MSBuild): `msbuild BinanceRJTech.sln /p:Configuration=Debug /m` (use `Release` for optimized builds).
- Build (Visual Studio): open `BinanceRJTech.sln`, select `x64` + `Debug/Release`, then Build.
- Run: from VS (F5) or `.\\x64\\Debug\\BinanceRJTech.exe` (path varies by config).
- Clean: `msbuild BinanceRJTech.sln /t:Clean`.

## Coding Style & Naming Conventions
- C++17+, 4-space indentation, UTF-8 encoding.
- Headers use `#pragma once`; prefer `.hpp/.cpp` pairs.
- Classes/types: PascalCase (`BinanceRest`); functions/vars: camelCase (`sendOrder`, `apiClient`); constants/macros: ALL_CAPS.
- One module per file pair; keep headers light and include only what you use.

## Testing Guidelines
- 현재 별도 테스트 스위트를 사용하지 않습니다.
- 변경 사항은 로컬 실행과 로그 확인으로 수동 검증하세요.
- 네트워크 I/O는 모의 데이터로 오프라인 재현 가능하게 유지합니다.

## Commit & Pull Request Guidelines
- Commits: imperative mood with scope prefix, e.g., `ws: fix reconnect backoff`, `rest: add signed request helper`.
- Include concise rationale and impact in the body; reference issues (`#123`).
- PRs: clear description, linked issues, reproduction steps, and screenshots for UI changes (ImGui). Keep changes focused and small.

## Communication & Permissions (에이전트 규칙)
- 소통 언어: 모든 이슈/PR/리뷰 및 에이전트 응답은 한국어를 기본으로 합니다.
- 승인 흐름: 작업 실행 시 별도 사용자 승인 질의 없이 필요한 권한을 요청해 진행합니다. 파괴적이거나 위험한 작업은 PR 설명에 명시하세요.
- 커밋/문서 언어: 커밋 메시지는 한국어 또는 영어 모두 허용(팀 합의 우선). PR/이슈 설명은 한국어 권장.

## Security & Configuration Tips
- Never commit API keys. Use env vars `BINANCE_API_KEY` and `BINANCE_API_SECRET` or your secure store.
- Keep TLS certs/keys in `cert/` locally; distribute via secure channels only.
- Verify REST and WebSocket endpoints via config constants before release.
