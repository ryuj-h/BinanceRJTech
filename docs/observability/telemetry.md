# Telemetry Logging Reference

## 로그 위치
- 기본 디렉터리: `logs/`
- 세션별 파일: `<session-name>-telemetry.log`
  - `console_app-telemetry.log`: 콘솔 진입점 (`Main.cpp`)
  - `gui_app-telemetry.log`: GUI 진입점 (`GuiAppMain.cpp`)

## 레코드 형식
```
[2025-09-18T12:34:56.123Z] TIMER gui.frame duration_us=8350 thread=1234
```
- `TIMER`: 스코프 기반 측정 (`telemetry::ScopedTimer`)
- `GAUGE`: FPS, 메시지 속도 등 실수형 값
- `COUNTER`: 누적 카운터 변화량
- `EVENT`: 상태 변화 정보, 예외 메시지

## 필수 지표
| 카테고리 | 라벨 | 설명 |
| --- | --- | --- |
| `gui` | `frame_time_ms` | 프레임당 렌더링 소요 시간 (목표 16ms 이하) |
| `gui` | `fps` | 실시간 프레임 레이트 (목표 60 이상) |
| `gui` | `order_updates_per_second` | 주문 데이터 업데이트 빈도 |
| `ws` | `messages_per_second` | 콘솔 앱 기준 초당 메시지 처리량 |
| `ws` | `receive_latency*` | 각 WebSocket 스레드의 수신 지연 |
| `rest` | `status_code` | 마지막 REST 응답 코드 |
| `rest` | `payload_bytes` | REST 응답 페이로드 크기 |

## 운영시 활용
- 스모크 테스트 후 로그를 압축해 PR 또는 릴리스 아티팩트로 첨부한다.
- 임계치 초과 현상이 발견되면 해당 타임스탬프 주변 로그를 분석하고, 버그 티켓에 링크한다.
- 장기 모니터링은 추후 Prometheus 내보내기 또는 ETW 연동 작업으로 확장할 수 있다.
