# OpenClaw T-Embed CC1101 Firmware

LilyGo T-Embed CC1101 보드를 OpenClaw Remote Gateway에 `node`로 연결하는 펌웨어입니다.

이 버전은 런타임 앱 구조를 사용합니다.

- `OpenClaw` 앱: 상태 확인 + Gateway 설정 + Save & Apply + Connect/Disconnect/Reconnect
- `Setting` 앱: Wi-Fi 설정 + BLE 스캔/연결/저장(재접속 대상) + System(Factory Reset)
- `File Explorer` 앱: SD 카드 마운트/용량 확인/디렉토리 탐색/텍스트 미리보기/Quick Format
- `Tailscale` 앱: Relay(host/port/path/ws/wss) 설정 + TCP 프로브 + Relay API Login/Logout/Status + Lite Direct(WireGuard, no relay) + OpenClaw URL 반영
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
- Relay 없이 사용할 수 있는 `Tailscale Lite`(WireGuard direct) 모드
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
- 저장 전에도 `Connect Now`로 즉시 연결 시도 가능

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

## Tailscale로 OpenClaw 연결

참고한 문서/프로젝트:

- [tailscale-iot](https://github.com/alfs/tailscale-iot)
- [Tailscale small binaries](https://tailscale.com/docs/how-to/set-up-small-tailscale)

이 펌웨어는 Tailscale 클라이언트를 직접 내장하지 않습니다. 대신 같은 LAN에 있는
소형 Tailscale 노드(라즈베리파이/OpenWrt 등)에서 TCP relay를 띄우면, 현재 펌웨어
수정 없이 OpenClaw Gateway를 Tailscale 경유로 사용할 수 있습니다.

`tailscale-iot`의 인증 개념(auth key + login server)은 이 프로젝트에서
**Relay API 로그인 방식**으로 반영했습니다.

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

선택: 디바이스에서 로그인 제어하려면 Relay API를 추가로 실행합니다.

```bash
./scripts/tailscale_relay_api.py --host 0.0.0.0 --port 9080 --base-path /api/tailscale
```

### 3) 디바이스 설정

1. `Setting -> Wi-Fi`에서 디바이스를 Relay와 같은 LAN에 연결
2. `OpenClaw -> Gateway`에서:
   - URL: `ws://<relay_lan_ip>:18789`
   - Auth Mode / Credential: OpenClaw Gateway와 동일하게 설정
3. `Tailscale` 앱에서:
   - `Relay API Host/IP`: `<relay_lan_ip>`
   - `Relay API Port`: `9080`
   - `Relay API Token`: (`RELAY_API_TOKEN` 또는 `--token-file` 사용 시 동일 값)
   - 로그인은 **Auth Key 필수** (`Relay Login`)
   - Auth Key는 직접 입력하거나 `Login from SD .env`로 SD 카드 `.env` 파일에서 선택 가능
   - `.env` 키 예시: `TAILSCALE_AUTH_KEY`, `TAILSCALE_AUTHKEY`, `TS_AUTHKEY`
   - (선택) `.env` 또는 UI의 `Login Server URL`로 Headscale URL 지정 가능
4. `OpenClaw -> Save & Apply`

### 4) 연결 확인

```bash
openclaw nodes status
openclaw nodes describe --node node-host
```

참고: `tailscale-iot`은 ESPHome/Headscale 중심의 별도 구현입니다. 이 레포는 Arduino 기반
OpenClaw 노드 펌웨어이므로, 동일 방식으로 직접 통합하지 않고 Relay 구성을 기본 경로로 제공합니다.

보안 주의: `tailscale_relay_api.py`는 로컬 네트워크 제어 API이므로,
필요 시 방화벽/분리망/토큰 헤더(`--token-file`)로 보호하세요.

## Relay 없이 Tailscale Lite (WireGuard direct)

`Tailscale` 앱에서 `Lite` 항목을 사용하면 Relay 없이 ESP32 내부 WireGuard 터널을 올릴 수 있습니다.
이 경로는 **데이터 경로에 relay가 필요 없습니다**.
보안 정책 일관성을 위해 Lite 모드도 `Auth Key`가 비어 있으면 저장/적용되지 않습니다.

- 메뉴:
  - `Lite Enabled`
  - `Lite Node IP`
  - `Lite Private Key`
  - `Lite Peer Host/IP`
  - `Lite Peer Port` (기본 `41641`)
  - `Lite Peer Public Key`
  - `Lite Connect` / `Lite Disconnect`
  - `Lite Load from SD .env`

`.env` 키 예시:

```env
TAILSCALE_AUTH_KEY=tskey-xxxxx
TAILSCALE_LOGIN_SERVER=https://headscale.example.com
TAILSCALE_LITE_NODE_IP=100.100.0.10
TAILSCALE_LITE_PRIVATE_KEY=xxxxx
TAILSCALE_LITE_PEER_HOST=your-peer.example.com
TAILSCALE_LITE_PEER_PORT=41641
TAILSCALE_LITE_PEER_PUBLIC_KEY=yyyyy
OPENCLAW_GATEWAY_URL=ws://100.100.0.1:8080
```

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
