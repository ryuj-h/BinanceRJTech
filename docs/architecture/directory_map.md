# 디렉터리 구성

```
apps/
  console/
    main.cpp                 # 실시간 주문서 스트리밍 콘솔 엔트리포인트
    legacy_depth_viewer.cpp  # 구형 CLI 뷰어 (레퍼런스 유지)
  gui/
    GuiAppMain.cpp           # ImGui 기반 GUI 엔트리포인트
  service/
    (비어 있음)             # 백그라운드 서비스 엔트리 예정
src/
  core/
    telemetry/PerfTelemetry.cpp
    ThreadPool.cpp           # 공용 스레드풀 실행 로직
  net/
    BinanceRest.cpp
    WebSocket.cpp
    AsyncWebSocketHub.cpp    # io_context 기반 비동기 WebSocket 허브
  ui/
    (비어 있음)             # ImGui 뷰/위젯 구현 영역
include/
  binancerj/
    core/BoundedQueue.hpp    # 제한 큐 템플릿
    core/ThreadPool.hpp      # 스레드풀 인터페이스
    telemetry/PerfTelemetry.hpp
    net/BinanceRest.hpp
    net/WebSocket.hpp
    net/AsyncWebSocketHub.hpp
assets/
  ui/imgui.ini
  ui/imgui_layout.ini
docs/ ...                   # 운영 가이드, 체크리스트, 런북 정리
tests/README.md              # 자동화 테스트 스텁 안내
third_party/
  boost/1.89.0/boost         # 부스트 헤더(버전 고정)
  imgui/...                  # Dear ImGui 소스 및 백엔드
tools/
  sync_boost.ps1             # 부스트 동기화 스크립트
```

현재는 `apps`(엔트리포인트)·`src`(공용 로직)·`include`(헤더 미러링)를 중심으로 구조가 정리되어 있으며, Phase 2 진행에 맞춰 비어 있는 영역은 점진적으로 채워질 예정이다.
