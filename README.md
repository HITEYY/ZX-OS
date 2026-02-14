# OpenClaw T-Embed CC1101 Firmware

LilyGo T-Embed CC1101 보드를 OpenClaw Remote Gateway에 `node`로 연결하는 펌웨어입니다.

이 버전은 런타임 앱 구조를 사용합니다.

- `OpenClaw` 앱: 상태 확인 + Gateway 설정 + Save & Apply + Connect/Disconnect/Reconnect
- `Setting` 앱: Wi-Fi 설정 + BLE 스캔/연결/저장(재접속 대상) + System(Factory Reset)
- `File Explorer` 앱: SD 카드 마운트/용량 확인/디렉토리 탐색/텍스트 미리보기/Quick Format
- `APPMarket` 앱: GitHub 최신 릴리스 조회/다운로드 + SD 패키지 관리 + 펌웨어 설치/재설치/백업

## 핵심 기능

- OpenClaw Gateway WebSocket 프로토콜(`req/res/event`) 호환
- `node.invoke.request` 처리
  - `system.which`
  - `system.run`
- `cc1101.info`
- `cc1101.set_freq`
- `cc1101.tx`
- 설정 영구 저장(SD: `/oc_cfg.json`, NVS 백업: namespace `oc_cfg`)
- Bruce 스타일 QWERTY 입력(온디바이스 키보드)
  - 전체 QWERTY 키보드 동시 표시 + `DONE/CAPS/DEL/SPACE/CANCEL`
  - ROT로 키 이동, OK로 입력, BACK으로 취소
- BLE 장치 스캔/연결
  - 저장 필드: `bleDeviceName`, `bleDeviceAddress`, `bleAutoConnect`

## 프로젝트 구조

- `src/core/runtime_config.*`: 설정 로드/저장/검증/초기화
- `src/core/gateway_client.*`: WS 연결/핸드셰이크/이벤트
- `src/core/node_command_handler.*`: invoke 명령 라우팅
- `src/core/cc1101_radio.*`: CC1101 제어
- `src/core/wifi_manager.*`: Wi-Fi 연결/스캔
- `src/core/ble_manager.*`: BLE 스캔/연결/상태
- `src/ui/ui_shell.*`: TFT/엔코더 UI 공통
- `src/apps/openclaw_app.*`: OpenClaw 앱
- `src/apps/settings_app.*`: Setting 앱
- `src/apps/file_explorer_app.*`: File Explorer 앱
- `src/apps/app_market_app.*`: APPMarket 앱
- `src/main.cpp`: 부트스트랩 + 런처 오케스트레이션

## 빌드/업로드

```bash
pio run -e t-embed-cc1101
pio run -e t-embed-cc1101 -t upload
pio device monitor -b 115200
```

## 기본값 시드

`include/user_config.h`는 **초기 시드**로만 사용됩니다.

- SD 카드(`/oc_cfg.json`) 저장값이 있으면 SD가 최우선입니다.
- SD 설정이 없거나 손상된 경우 NVS 백업을 사용합니다.
- SD/NVS 모두 비어있을 때만 `user_config.h` 값이 로드됩니다.

## 앱 설정 흐름

1. `Setting -> Wi-Fi`
- `Scan Networks`로 SSID 선택 + 비밀번호 입력
- `Hidden SSID`로 수동 SSID/비밀번호 입력
- SSID/비밀번호 변경 시 즉시 저장(SD `/oc_cfg.json` + NVS backup)
- `Connect Now`로 즉시 재연결 시도 가능

2. `OpenClaw -> Gateway`
- URL 입력 (`ws://` 또는 `wss://`)
- Auth Mode 선택 (`Token` / `Password`)
- Credential 입력(마스킹)

3. `OpenClaw -> Save & Apply`
- 유효성 검사 후 SD(`/oc_cfg.json`) 저장 + NVS 백업
- Wi-Fi/Gateway 런타임 반영
- Gateway 재연결 시도

4. `Setting -> BLE`
- `Scan & Connect`, `Connect Saved`, `Disconnect`
- `Edit Device Addr/Name`, `Auto Connect`, `Forget Saved`

5. `Setting -> System -> Factory Reset`
- 2단계 확인 후 SD 설정 파일 + NVS 백업 설정 삭제

## APPMarket 사용

`APPMarket` 앱에서 펌웨어 배포본을 GitHub에서 받아 SD로 관리하고 설치할 수 있습니다.

1. `GitHub Repo`에 `owner/repo` 형식 입력
2. (선택) `Release Asset`에 원하는 `.bin` 파일명 입력
   - 비워두면 latest release의 첫 `.bin` 자산을 자동 선택
3. `Check Latest`로 최신 tag/asset 확인
4. `Download Latest to SD`로 `/appmarket/latest.bin` 저장
5. `Install Latest`로 최신 버전 덮어쓰기(재부팅)

추가 기능:
- `Backup Running App to SD`: 현재 실행 중 펌웨어를 `/appmarket/current_backup.bin`으로 전송
- `Reinstall from Backup`: 백업본 재설치
- `Install from SD .bin`: SD 카드의 임의 `.bin` 선택 설치
- `Delete Latest Package` / `Delete Backup Package`: SD 패키지 삭제
- `Save Config`: APPMarket 설정(repo/asset) 저장

## BLE 연결 범위

- 이 펌웨어의 BLE 연결은 **BLE GATT 중앙(Central) 연결**입니다.
- 일반적인 오디오 이어폰/마이크의 A2DP/HFP(클래식 BT 오디오)는 범위 밖입니다.

## OpenClaw 테스트

```bash
openclaw nodes status
openclaw nodes describe --node node-host

openclaw nodes invoke --node node-host --command cc1101.info --params "{}"
openclaw nodes invoke --node node-host --command cc1101.set_freq --params '{"mhz":433.92}'
openclaw nodes invoke --node node-host --command cc1101.tx --params '{"code":"0xABCDEF","bits":24,"pulseLength":350,"protocol":1,"repeat":10}'

openclaw nodes invoke --node node-host --command system.run --params '{"command":["cc1101.info"]}'
```

## Gateway allowlist 주의

`cc1101.*` 직접 명령은 Gateway 정책에서 차단될 수 있습니다.
필요 시 아래 항목을 추가하세요.

```toml
[gateway.nodes]
allowCommands = ["cc1101.info", "cc1101.set_freq", "cc1101.tx"]
```

## 법적 주의

무선 송신/재전송은 지역 법규를 준수해서 사용해야 합니다.
