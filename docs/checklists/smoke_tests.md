# Smoke Test Checklist

## 사전 준비
- `BINANCE_API_KEY` / `BINANCE_API_SECRET` 환경 변수를 샌드박스 키로 설정한다.
- `logs/` 디렉터리를 비우거나 백업해 이번 실행 로그만 수집한다.
- 네트워크 연결과 VPN 설정을 확인하고, 거래소 엔드포인트 접근이 가능한지 `ping fstream.binance.com`으로 확인한다.

## 콘솔 앱 (`Main.cpp`)
1. `msbuild BinanceRJTech.sln /p:Configuration=Debug /m`으로 빌드한다.
2. `.\x64\Debug\BinanceRJTech.exe`를 실행한다.
3. 초당 메시지(`msgs/s`)가 30 이상으로 유지되는지 확인하고, `Ctrl+C` 종료 시 정상적으로 종료 로그가 남는지 확인한다.
4. `logs/console_app-telemetry.log`에 `messages_per_second`, `receive_latency` 항목이 기록되는지 확인한다.

## GUI 앱 (`GuiAppMain.cpp`)
1. `GuiAppMain` 구성을 빌드하고 실행한다.
2. 차트가 5초 이내에 업데이트되는지, FPS가 60 이상으로 유지되는지 UI 좌측 상단에서 확인한다.
3. 주문북, 체결, 계좌 정보가 UI에 표시되는지 확인하고 레이아웃 저장(`imgui_layout.ini`)이 갱신되는지 확인한다.
4. `logs/gui_app-telemetry.log`에서 `frame_time_ms`, `fps`, `order_updates_per_second` 항목을 확인하고, 16ms/60fps 기준을 벗어나는 프레임이 있는지 검토한다.

## REST 클라이언트 (`BinanceRest.cpp`)
1. `BinanceRest` 샘플 함수를 호출해 서버 시간(`getServerTime`)을 요청한다.
2. HTTP 2xx 코드 수신 여부와 오류 시 재시도 정책이 동작하는지 확인한다.
3. `logs/gui_app-telemetry.log` 또는 해당 세션 로그에서 `rest.status_code`, `rest.payload_bytes` 항목을 확인해 응답 크기와 상태 코드를 기록한다.

## 회귀 판정
- 위 항목 중 하나라도 실패하면 변경 사항을 되돌리고 원인 분석 후 재시도한다.
- 모든 테스트가 통과한 경우 결과 로그와 요약을 PR 설명에 첨부한다.
