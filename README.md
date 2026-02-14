# OpenClaw T-Embed CC1101 Firmware

LilyGo T-Embed CC1101 보드를 OpenClaw Remote Gateway에 `node`로 연결하는 펌웨어입니다.

이 버전은 런타임 앱 구조를 사용합니다.

- `OpenClaw` 앱: 상태 확인 + Gateway 설정 + Save & Apply + Connect/Disconnect/Reconnect
- `Setting` 앱: Wi-Fi 설정 + BLE 스캔/연결/저장(재접속 대상) + System(Factory Reset)
- `File Explorer` 앱: SD 카드 마운트/용량 확인/디렉토리 탐색/텍스트 미리보기
- `Tailscale` 앱: Relay(host/port/path/ws/wss) 설정 + TCP 프로브 + OpenClaw URL 반영

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
- `src/apps/tailscale_app.*`: Tailscale 앱
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

## 앱 설정 흐름

1. `Setting -> Wi-Fi`
- `Scan Networks`로 SSID 선택 + 비밀번호 입력
- `Hidden SSID`로 수동 SSID/비밀번호 입력

2. `OpenClaw -> Gateway`
- URL 입력 (`ws://` 또는 `wss://`)
- Auth Mode 선택 (`Token` / `Password`)
- Credential 입력(마스킹)

3. `OpenClaw -> Save & Apply`
- 유효성 검사 후 NVS 저장
- Wi-Fi/Gateway 런타임 반영
- Gateway 재연결 시도

4. `Setting -> BLE`
- `Scan & Connect`, `Connect Saved`, `Disconnect`
- `Edit Device Addr/Name`, `Auto Connect`, `Forget Saved`

5. `Setting -> System -> Factory Reset`
- 2단계 확인 후 NVS 설정 삭제

## Tailscale로 OpenClaw 연결

참고한 문서/프로젝트:

- [tailscale-iot](https://github.com/alfs/tailscale-iot)
- [Tailscale small binaries](https://tailscale.com/docs/how-to/set-up-small-tailscale)

이 펌웨어는 Tailscale 클라이언트를 직접 내장하지 않습니다. 대신 같은 LAN에 있는
소형 Tailscale 노드(라즈베리파이/OpenWrt 등)에서 TCP relay를 띄우면, 현재 펌웨어
수정 없이 OpenClaw Gateway를 Tailscale 경유로 사용할 수 있습니다.

연결 구조:

```text
T-Embed(ESP32) --ws://LAN_RELAY_IP:18789--> Relay 노드 --Tailscale--> OpenClaw Gateway
```

### 1) Relay 노드 준비

- Relay 노드에 Tailscale을 설치하고 tailnet에 조인합니다.
- Relay 노드에서 Gateway tailnet 주소(예: `100.x.y.z` 또는 `*.ts.net`)로 연결 가능한지 확인합니다.

### 2) Relay 실행

이 레포에 포함된 스크립트를 Relay 노드에서 실행합니다.

```bash
./scripts/tailscale_openclaw_relay.sh <gateway_tailnet_host_or_ip> 18789 18789
```

예시:

```bash
./scripts/tailscale_openclaw_relay.sh openclaw-gateway.tailnet.ts.net
```

### 3) 디바이스 설정

1. `Setting -> Wi-Fi`에서 디바이스를 Relay와 같은 LAN에 연결
2. `OpenClaw -> Gateway`에서:
   - URL: `ws://<relay_lan_ip>:18789`
   - Auth Mode / Credential: OpenClaw Gateway와 동일하게 설정
3. `OpenClaw -> Save & Apply`

### 4) 연결 확인

```bash
openclaw nodes status
openclaw nodes describe --node node-host
```

참고: `tailscale-iot`은 ESPHome/Headscale 중심의 별도 구현입니다. 이 레포는 Arduino 기반
OpenClaw 노드 펌웨어이므로, 동일 방식으로 직접 통합하지 않고 Relay 구성을 기본 경로로 제공합니다.

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
