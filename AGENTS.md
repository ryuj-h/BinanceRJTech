# Repository Guidelines

BinanceRJTech는 GUI, 콘솔, 백그라운드 서비스를 아우르는 C++17 프로젝트입니다. Windows/MSBuild 환경을 기준으로 하며, 멀티스레딩과 실시간 렌더링 성능을 유지하기 위해 본 지침을 따르세요.

## Project Structure & Module Organization
- `apps/` 폴더에 모든 엔트리포인트가 위치합니다 (`apps/console/main.cpp`, `apps/gui/GuiAppMain.cpp`, `apps/console/legacy_depth_viewer.cpp`).
- 공용 로직은 `src/` (예: `src/net/BinanceRest.cpp`, `src/core/telemetry/PerfTelemetry.cpp`)에, 대응 헤더는 `include/binancerj/` 하위에 둡니다.
- 자산(`assets/ui/imgui*.ini`), 문서(`docs/`), 도구(`tools/`), 테스트 스텁(`tests/README.md`)을 분리하여 추후 자동화와 디자인 작업을 확장할 수 있게 합니다.
- 벤더 종속성은 `third_party/` 아래 버전별로 보관합니다 (예: `third_party/boost/1.89.0/boost`). `tools/sync_boost.ps1`로 갱신 절차를 자동화하세요.

## Build, Test, and Development Commands
- `msbuild BinanceRJTech.sln /p:Configuration=Debug /m` 으로 전체 솔루션을 병렬 빌드합니다. `Configuration=Release`로 전환 시 배포용 바이너리를 생성합니다.
- `msbuild BinanceRJTech.sln /t:Clean` 으로 중간 산출물을 정리한 뒤 깨끗한 빌드를 수행하세요.
- GUI 실행: Visual Studio `F5` 또는 `.\x64\Debug\BinanceRJTech.exe`. 콘솔 실행: `.\x64\Debug\BinanceRJTech.exe /entry console` (프로젝트 설정에 따라 엔트리포인트 선택).
- 실험용 컴파일러 플래그는 `.vcxproj.user` 또는 `build/` 하위 스크립트에 격리해 공유 솔루션 변경을 최소화합니다.

## Coding Style & Naming Conventions
- C++17, 4-스페이스 들여쓰기, UTF-8(Windows CRLF) 유지.
- 헤더는 `#pragma once` 사용, `include/binancerj/<domain>` 네임스페이스 구조를 따릅니다.
- 타입은 `PascalCase`, 함수/변수는 `camelCase`, 상수/매크로는 `ALL_CAPS`. RAII와 `std::` 라이브러리를 우선 사용하세요.

## Testing Guidelines
- 현재 자동화된 테스트는 없으며, `docs/checklists/smoke_tests.md`에 정의된 스모크 시나리오를 수동 실행합니다.
- 네트워크 기능은 샌드박스 API 키와 모의 응답으로 재현 가능하게 유지하고, 검증 로그를 `logs/`에 보관하세요.
- 신규 기능이나 버그 수정 시 수동 검증 절차를 PR 본문에 명시합니다.

## Commit & Pull Request Guidelines
- 커밋 메시지는 `scope: imperative message` 형식을 유지합니다 (예: `ws: add latency telemetry`).
- PR에는 변경 요약, 검증 로그 링크, 스크린샷(그래픽 변경 시), 잔존 리스크를 포함하세요.
- 멀티스레딩·성능 민감 변경은 2인 리뷰와 추가 로그 캡처를 요구합니다.

## Security & Configuration Tips
- API 키는 환경 변수(`BINANCE_API_KEY`, `BINANCE_API_SECRET`)나 보안 저장소에서 로드합니다.
- TLS 자재는 `cert/`에 보관하며 Git에 커밋하지 마세요. 필요 시 안전 채널로 배포합니다.
- 배포 전 `rest`/`ws` 엔드포인트 상수, 텔레메트리 임계값(FPS/지연)이 예상 범위인지 확인하세요.
