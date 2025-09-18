# Directory Layout

```
apps/
  console/
    main.cpp                 # 텔레메트리 계측이 적용된 콘솔 엔트리포인트
    legacy_depth_viewer.cpp  # 기존 CLI 북 뷰어 (추후 통합 예정)
  gui/
    GuiAppMain.cpp           # ImGui 기반 GUI 엔트리포인트
src/
  core/
    telemetry/PerfTelemetry.cpp
  net/
    BinanceRest.cpp
    WebSocket.cpp
include/
  binancerj/
    telemetry/PerfTelemetry.hpp
    net/BinanceRest.hpp
    net/WebSocket.hpp
assets/
  ui/imgui.ini
  ui/imgui_layout.ini
docs/ ...                   # 운영, 체크리스트, 아키텍처 문서
tests/README.md              # 자동화 테스트 스텁
third_party/
  boost/1.89.0/boost         # 부스트 배포본
  imgui/...                  # Dear ImGui
```

각 모듈은 `apps`(엔트리포인트) ↔ `src`(로직) ↔ `include`(공용 헤더) 구조를 따른다.
