# Crash Dump & Rollback Runbook

## 목적
런타임 크래시 발생 시 덤프를 수집하고, 안전하게 롤백하여 서비스 영향을 최소화한다.

## 덤프 수집
1. 레지스트리에 사용자 모드 덤프를 등록한다.
   ```powershell
   New-Item -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" -Force | Out-Null
   New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" -Name DumpFolder -PropertyType ExpandString -Value "%LOCALAPPDATA%\CrashDumps" -Force | Out-Null
   New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" -Name DumpType -PropertyType DWord -Value 2 -Force | Out-Null
   ```
2. 크래시가 재현되면 `%LOCALAPPDATA%\CrashDumps` 폴더에서 덤프(`.dmp`) 파일을 확보한다.
3. `WinDbg` 또는 `cdb`로 분석한다.
   ```cmd
   windbgx -z "%LOCALAPPDATA%\CrashDumps\BinanceRJTech.exe.<pid>.dmp"
   !analyze -v
   ```

## 텔레메트리 확보
- `logs/*-telemetry.log`를 보관하여 크래시 이전의 FPS, 네트워크 지연 추이를 확인한다.
- Windows 이벤트 뷰어에서 `Application` 로그를 내보내어 예외 스택 정보를 확보한다.

## 롤백 절차
1. 문제가 된 빌드의 Git 커밋 해시를 기록한다.
2. `git revert <commit>` 또는 안정 태그로 체크아웃해 정상 동작을 복원한다.
3. 롤백 후 `docs/checklists/smoke_tests.md`에 정의된 스모크 테스트를 다시 수행한다.
4. 문제 원인을 분석한 결과와 덤프, 텔레메트리, 재현 절차를 이슈 트래커에 첨부한다.

## 재발 방지
- 크래시 원인에 해당하는 단위 테스트 또는 회귀 테스트를 추가한다.
- 텔레메트리 임계치(프레임 타임, 네트워크 지연)를 초과할 경우 경고를 띄우도록 후속 작업을 계획한다.
