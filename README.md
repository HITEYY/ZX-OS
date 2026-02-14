# OpenClaw T-Embed CC1101 Firmware

LilyGo T-Embed CC1101 보드를 OpenClaw Remote Gateway에 `node`로 연결하는 펌웨어입니다.

이 버전은 런타임 앱 구조를 사용합니다.

- `OpenClaw` 앱: 상태 확인 + Connect/Disconnect/Reconnect
- `Setting` 앱: Wi-Fi/Gateway 설정 편집, NVS 저장, Factory Reset
- `Setting` 앱: BLE 스캔/연결/저장(재접속 대상)

## 핵심 기능

- OpenClaw Gateway WebSocket 프로토콜(`req/res/event`) 호환
- `node.invoke.request` 처리
  - `system.which`
  - `system.run`
  - `cc1101.info`
  - `cc1101.set_freq`
  - `cc1101.tx`
- 설정 영구 저장(NVS, namespace: `oc_cfg`)
- Bruce 스타일 QWERTY 입력(온디바이스 키보드)
  - 4-row keyset + `DONE/CAPS/DEL/SPACE/CANCEL`
  - Back 버튼으로 row 전환
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
- `src/main.cpp`: 부트스트랩 + 런처 오케스트레이션

## 빌드/업로드

```bash
pio run -e t-embed-cc1101
pio run -e t-embed-cc1101 -t upload
pio device monitor -b 115200
```

## 기본값 시드

`include/user_config.h`는 **초기 시드**로만 사용됩니다.

- NVS 저장값이 있으면 NVS가 우선됩니다.
- NVS가 비어있을 때만 `user_config.h` 값이 로드됩니다.

## 설정 앱 흐름

1. `Setting -> Wi-Fi`
- `Scan Networks`로 SSID 선택 + 비밀번호 입력
- `Hidden SSID`로 수동 SSID/비밀번호 입력

2. `Setting -> Gateway`
- URL 입력 (`ws://` 또는 `wss://`)
- Auth Mode 선택 (`Token` / `Password`)
- Credential 입력(마스킹)

3. `Setting -> Save & Apply`
- 유효성 검사 후 NVS 저장
- Wi-Fi/Gateway 런타임 반영
- Gateway 재연결 시도

4. `Setting -> BLE`
- `Scan & Connect`, `Connect Saved`, `Disconnect`
- `Edit Device Addr/Name`, `Auto Connect`, `Forget Saved`

5. `Setting -> System -> Factory Reset`
- 2단계 확인 후 NVS 설정 삭제

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
