# OpenClaw T-Embed CC1101 Firmware

LilyGo T-Embed CC1101 보드를 OpenClaw Remote Gateway에 `node`로 연결하는 펌웨어입니다.

이 버전은 **LVGL 기반 런타임 UI 구조**를 사용합니다.

- `OpenClaw` 앱: 상태 확인 + Gateway 설정 + Messenger(채팅/파일/음성) + Save & Apply + Connect/Disconnect/Reconnect
- `Setting` 앱: Wi-Fi 설정 + BLE 스캔/연결/저장(재접속 대상) + System(Device Name/UI/Timezone/Factory Reset) + Firmware Update(독립 업데이트 메뉴)
- `File Explorer` 앱: SD 카드 마운트/용량 확인/디렉토리 탐색/텍스트 미리보기/이미지 보기(png/jpg/jpeg/bmp)/오디오 재생(wav/mp3/ogg 등)/Quick Format
- `APPMarket` 앱: GitHub 최신 릴리스 조회/다운로드 + SD 패키지 관리 + 펌웨어 설치/재설치/백업
- `RF` 앱: CC1101 고급 설정(변조/채널/속도/편이/대역폭/패킷) + 패킷 TX/RX + RSSI + OOK TX
- `NFC` 앱: PN532(I2C) 모듈 감지 + 태그 UID 스캔
- `RFID` 앱: RC522(SPI) 모듈 감지 + 카드 UID 스캔
- `NRF24` 앱: nRF24L01(SPI) 모듈 감지 + 채널/속도/출력 설정 + 텍스트 TX/RX

## 핵심 기능

- OpenClaw Gateway WebSocket 프로토콜(`req/res/event`) 호환
- 프로토콜 v3 핸드셰이크(`connect.challenge` + `hello-ok`) 대응
- `ws://` / `wss://` 연결 지원
- `node.invoke.request` 처리
  - `system.which`
  - `system.run`
- `cc1101.info`
- `cc1101.set_freq`
- `cc1101.tx`
- `cc1101.read_rssi`
- `cc1101.packet_get`
- `cc1101.packet_set`
- `cc1101.packet_tx_text`
- `cc1101.packet_rx_once`
- CC1101 packet mode 설정/송수신/RSSI 측정
- Messaging event 송신/수신
  - 텍스트 요청: `agent.request` (`node.event`)
  - 채팅 세션 구독: `chat.subscribe` / `chat.unsubscribe` (`node.event`)
  - 채팅 스트림 수신: `chat` (delta/final/error)
  - 파일/음성 첨부(기본): `chat.send` + `attachments` (base64 payload, 소용량 우선)
  - 첨부가 큰 경우: `agent.request` 텍스트 프레이밍(`[ATTACHMENT_BEGIN]` / `[ATTACHMENT_CHUNK]` / `[ATTACHMENT_END]`) 폴백
  - 레거시 미디어 이벤트(`msg.file.meta/chunk`, `msg.voice.meta/chunk`)는 컴파일 옵션으로만 사용
- Messenger 발신 대상 고정: `USER_OPENCLAW_DEFAULT_AGENT_ID` (기본값 `default`)
- 설정 영구 저장(SD: `/oc_cfg.json`, NVS 백업: namespace `oc_cfg`)
- Bruce 스타일 QWERTY 입력(온디바이스 키보드)
  - 전체 QWERTY 키보드 동시 표시 + `DONE/CAPS/DEL/SPACE/CANCEL`
  - ROT로 키 이동, OK로 입력, BACK으로 취소
- BLE 장치 스캔/연결
  - 저장 필드: `bleDeviceAddress`, `bleAutoConnect`
- 공통 장치명(Device Name) 설정
  - 저장 필드: `deviceName`
  - 적용 범위: OpenClaw `client.displayName`, BLE stack 이름
- BLE HID 키보드 입력 수신
  - `Setting -> BLE -> Keyboard Input View`에서 입력 확인
  - `Setting -> BLE -> Clear Keyboard Input`으로 버퍼 초기화
- MIC(ADC/PDM) 직접 녹음 후 음성 메시지 전송
  - `OpenClaw -> Messenger -> Record Voice` (BLE 우선, 미지원 시 MIC 폴백)
  - BLE notify 기반 오디오 스트림 수신 시 `.wav` 저장 후 정책 기반 첨부 전송
  - 녹음된 파일은 SD에 `.wav`로 저장 후 정책 기반 첨부 전송

## LVGL UI 아키텍처

- 디스플레이 백엔드: `TFT_eSPI` 유지 + 상위 렌더링 계층만 `LVGL` 적용
- UI 테마: 다크 운영형, 최소 모션(짧은 전환 시간)
- 언어 정책: `English` 기본, `English/Korean` 토글 지원
- 입력 장치: 터치 미사용, 엔코더 + 버튼 전용
- 라우팅: 런처 중심 앱 내비게이션 (`UiNavigator`)
- UI 런타임: 공통 화면/입력/토스트/텍스트 입력 (`UiRuntime`)
- SPI 버스: TFT/SD/CC1101 공용 SPI 초기화/접근 계층(`shared_spi_bus`) 사용

### 입력 조작

- 엔코더 회전: 항목 이동
- `OK` 짧게: 선택/엔터 (`ENTER`)
- `BACK` 또는 `OK` 길게: 취소/뒤로 (`ESC`)
- 상단 버튼 3초 홀드: deep sleep 진입 (기존 동작 유지)

### 언어 설정

- 경로: `Setting -> System -> UI Language`
- 선택값: `English`, `Korean`
- 저장: 설정 저장 시 SD(`/oc_cfg.json`) + NVS 백업 반영
- 재부팅 후 유지: `uiLanguage` 필드(`en`/`ko`)로 영속화

## 프로젝트 구조

- `src/core/runtime_config.*`: 설정 로드/저장/검증/초기화
- `src/core/gateway_client.*`: WS 연결/핸드셰이크/이벤트
- `src/core/node_command_handler.*`: invoke 명령 라우팅
- `src/core/cc1101_radio.*`: CC1101 제어
- `src/core/shared_spi_bus.*`: TFT/SD/CC1101 공용 SPI 버스 관리
- `src/core/wifi_manager.*`: Wi-Fi 연결/스캔
- `src/core/ble_manager.*`: BLE 스캔/연결/상태
- `src/core/audio_recorder.*`: MIC(ADC/PDM) WAV 녹음
- `src/core/ble_manager.*`: BLE 연결/키보드 + BLE 오디오 스트림 수신
- `src/ui/lvgl_port.*`: LVGL 포팅 계층(TFT flush, draw buffer, tick pump)
- `src/ui/input_adapter.*`: 엔코더/버튼 -> LVGL indev 어댑터
- `src/ui/ui_runtime.*`: 공통 UI 런타임(메뉴/정보/확인/입력/토스트)
- `src/ui/ui_navigator.*`: 런처 라우팅/앱 진입
- `src/ui/i18n.*`: 다국어 문자열 리소스(en/ko)
- `src/apps/openclaw_app.*`: OpenClaw 앱
- `src/apps/settings_app.*`: Setting 앱
- `src/apps/file_explorer_app.*`: File Explorer 앱
- `src/apps/app_market_app.*`: APPMarket 앱
- `src/apps/rf_app.*`: RF 앱(CC1101 고급 제어)
- `src/apps/nfc_app.*`: NFC 앱(PN532)
- `src/apps/rfid_app.*`: RFID 앱(RC522)
- `src/apps/nrf24_app.*`: NRF24 앱
- `src/main.cpp`: 부트스트랩 + 런처 오케스트레이션

## 빌드/업로드

```bash
pio run -e t-embed-cc1101
pio run -e t-embed-cc1101 -t upload
pio device monitor -b 115200
```

## 펌웨어/앱 릴리즈 워크플로우

- `.github/workflows/build.yml`: push/PR마다 펌웨어 + 앱 패키지 아티팩트 생성
- `.github/workflows/release.yml`: 태그/수동 실행 시 GitHub Release 자동 생성

1. 태그 기반 릴리즈
```bash
git tag v1.0.0
git push origin v1.0.0
```
2. 수동 릴리즈
- GitHub Actions에서 `Release Firmware and App Package` 실행
- 입력값: `tag` (예: `v1.0.0`), 필요 시 `prerelease`, `draft`

생성 자산:
- `openclaw-t-embed-cc1101-<tag>.bin`
- `openclaw-t-embed-cc1101-<tag>.bin.sha256`
- `openclaw-t-embed-cc1101-latest.bin`
- `openclaw-t-embed-cc1101-latest.bin.sha256`
- `openclaw-app-t-embed-cc1101-<tag>.json`
- `openclaw-app-t-embed-cc1101-<tag>.zip`
- `openclaw-app-t-embed-cc1101-<tag>.zip.sha256`

## 기본값 시드

`include/user_config.h`는 **초기 시드**로만 사용됩니다.

- SD 카드(`/oc_cfg.json`) 저장값이 있으면 SD가 최우선입니다.
- SD 설정이 없거나 손상된 경우 NVS 백업을 사용합니다.
- SD/NVS 모두 비어있을 때만 `user_config.h` 값이 로드됩니다.
- SD 루트 `/.env`에 Gateway 값이 있으면 부팅 시 최종값을 덮어씁니다.
- Messenger 기본 수신자(기본 에이전트)는 `USER_OPENCLAW_DEFAULT_AGENT_ID`로 설정합니다.
- MIC 녹음 사용 시 `USER_MIC_ADC_PIN`에 ADC 가능한 핀 번호를 설정합니다.

`/.env` 지원 키:
- `OPENCLAW_GATEWAY_URL` (또는 `GATEWAY_URL`)
- `OPENCLAW_GATEWAY_TOKEN` (또는 `GATEWAY_TOKEN`)
- `OPENCLAW_GATEWAY_PASSWORD` (또는 `GATEWAY_PASSWORD`)
- 선택: `OPENCLAW_GATEWAY_AUTH_MODE` (또는 `GATEWAY_AUTH_MODE`, `token|password|0|1`)
- 선택: `OPENCLAW_GATEWAY_DEVICE_TOKEN` (또는 `GATEWAY_DEVICE_TOKEN`)
- 선택: `OPENCLAW_GATEWAY_DEVICE_ID` (또는 `GATEWAY_DEVICE_ID`)
- 선택: `OPENCLAW_GATEWAY_DEVICE_PUBLIC_KEY` (또는 `GATEWAY_DEVICE_PUBLIC_KEY`)
- 선택: `OPENCLAW_GATEWAY_DEVICE_PRIVATE_KEY` (또는 `GATEWAY_DEVICE_PRIVATE_KEY`)

## 외부 모듈 기본 핀

기본값은 `include/user_config.h`에 있으며, 실제 배선에 맞게 수정하세요.

- PN532 (I2C)
  - `USER_NFC_I2C_SDA=8`
  - `USER_NFC_I2C_SCL=18`
  - `USER_NFC_IRQ_PIN=7`
  - `USER_NFC_RESET_PIN=42`
- RC522 (SPI)
  - `USER_RFID_SS_PIN=2`
  - `USER_RFID_RST_PIN=1`
- nRF24L01 (SPI)
  - `USER_NRF24_CE_PIN=17`
  - `USER_NRF24_CSN_PIN=14`
  - `USER_NRF24_CHANNEL=76`
  - `USER_NRF24_DATA_RATE=1` (0:250kbps, 1:1Mbps, 2:2Mbps)
  - `USER_NRF24_PA_LEVEL=1` (0:MIN, 1:LOW, 2:HIGH, 3:MAX)
- MIC (ADC/PDM)
  - `USER_MIC_ADC_PIN=-1` (`-1`은 비활성, 실제 핀 번호 설정 시 활성)
  - `USER_MIC_PDM_DATA_PIN=42`, `USER_MIC_PDM_CLK_PIN=39` (온보드 PDM MIC 기본값)
  - `USER_MIC_SAMPLE_RATE=8000`
  - `USER_MIC_DEFAULT_SECONDS=5`
  - `USER_MIC_MAX_SECONDS=30`
- BLE Audio (optional UUID filter)
  - `USER_BLE_AUDIO_SERVICE_UUID=""`
  - `USER_BLE_AUDIO_CHAR_UUID=""`

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

4. `OpenClaw -> Messenger`
- `Write Message`: 텍스트 메시지 전송
  - 내부적으로 `agent.request` + `chat.subscribe`를 사용해 Agent 실시간 응답 수신
- `Send File (SD)`: SD 일반 파일 전송(최대 4MB)
  - 라우팅 정책:
    - 96KB 이하: `chat.send` + `attachments`
    - 이미지(`image/*`) + 512KB 이하: `agent.request` framed
    - 비이미지(`pdf/zip/bin/json/txt` 등): text fallback
    - 512KB 초과: text fallback
    - framed 실패 시: `USER_MESSENGER_ENABLE_LEGACY_MEDIA_FALLBACK=1`일 때만 `msg.file.meta/chunk`
- `Record Voice`: BLE 오디오 우선
  - BLE 스트림 특성(Notify/Indicate) 발견 시 BLE 오디오를 녹음 후 전송
  - BLE 스트림 미발견/미연결 시 장치 MIC(ADC/PDM)로 자동 폴백
  - 녹음 시간 입력 없이 `OK` 또는 `BACK` 버튼으로 녹음 종료
- `Send Voice File (SD)`: SD 오디오 파일(`.wav/.mp3/.m4a/.aac/.opus/.ogg`) 전송(최대 2MB)
  - 라우팅 정책:
    - 96KB 이하: `chat.send` + `attachments`
    - 96KB 초과 ~ 512KB 이하: `agent.request` framed
    - 512KB 초과: text fallback
    - framed 실패 시: `USER_MESSENGER_ENABLE_LEGACY_MEDIA_FALLBACK=1`일 때만 `msg.voice.meta/chunk`
- 채팅 로그: 송신/수신 메시지 통합 보기 + 상세 보기 + 로그 삭제
- 모든 발신(`text/file/voice`)은 `USER_OPENCLAW_DEFAULT_AGENT_ID`로 전송

5. `Setting -> BLE`
- `Scan & Connect`, `Connect Saved`, `Disconnect`
- `Keyboard Input View`, `Clear Keyboard Input`
- `Edit Device Addr`, `Auto Connect`, `Forget Saved`

6. `Setting -> System`
- `Device Name`: 공통 장치명 변경 (OpenClaw/BLE 공통 반영)
- `UI Language`: `English/Korean` 전환
- `Factory Reset`: 2단계 확인 후 SD 설정 파일 + NVS 백업 설정 삭제

7. `Setting -> Firmware Update`
- 독립 펌웨어 업데이트 메뉴에서 최신 릴리스 확인/다운로드/설치 수행
- 다운로드 저장 위치: `/firmware/latest.bin`

## APPMarket 사용

`APPMarket` 앱에서 펌웨어 배포본을 GitHub에서 받아 SD로 관리하고 설치할 수 있습니다.

1. APPMarket 저장소는 고정값 `HITEYY/AI-cc1101`을 사용합니다.
2. (선택) `Release Asset`에 원하는 `.bin` 파일명 입력
   - 기본 권장값: `openclaw-t-embed-cc1101-latest.bin`
   - 비워두면 latest release의 첫 `.bin` 자산을 자동 선택
3. `Check Latest`로 최신 tag/asset 확인
4. `Download Latest to SD`로 `/appmarket/latest.bin` 저장
5. `Install Latest`로 최신 버전 덮어쓰기(재부팅)

추가 기능:
- `Backup Running App to SD`: 현재 실행 중 펌웨어를 `/appmarket/current_backup.bin`으로 전송
- `Reinstall from Backup`: 백업본 재설치
- `Install from SD .bin`: SD 카드의 임의 `.bin` 선택 설치
- `Delete Latest Package` / `Delete Backup Package`: SD 패키지 삭제
- `Save Config`: APPMarket 설정(asset) 저장

## BLE 연결 범위

- 이 펌웨어의 BLE 연결은 **BLE GATT 중앙(Central) 연결**입니다.
- BLE HID 키보드는 입력 수신까지 지원합니다.
- 이어폰/마이크/스피커는 연결 시도는 가능하지만, 오디오 스트리밍(A2DP/HFP/LE Audio)은 지원하지 않습니다.

## 성능 참고값

아래 값은 `t-embed-cc1101` 릴리스 빌드 기준 참고치입니다.

- RAM: 약 `40.6%` (`132,964 / 327,680 bytes`)
- Flash: 약 `32.0%` (`2,099,827 / 6,553,600 bytes`)

## OpenClaw 테스트

```bash
openclaw nodes status
openclaw nodes describe --node node-host

openclaw nodes invoke --node node-host --command cc1101.info --params "{}"
openclaw nodes invoke --node node-host --command cc1101.set_freq --params '{"mhz":433.92}'
openclaw nodes invoke --node node-host --command cc1101.tx --params '{"code":"0xABCDEF","bits":24,"pulseLength":350,"protocol":1,"repeat":10}'
openclaw nodes invoke --node node-host --command cc1101.read_rssi --params "{}"
openclaw nodes invoke --node node-host --command cc1101.packet_get --params "{}"
openclaw nodes invoke --node node-host --command cc1101.packet_set --params '{"modulation":2,"channel":0,"dataRateKbps":4.8,"deviationKHz":5,"rxBandwidthKHz":256,"syncMode":2,"packetFormat":0,"crcEnabled":true,"lengthConfig":1,"packetLength":61,"whitening":false,"manchester":false}'
openclaw nodes invoke --node node-host --command cc1101.packet_tx_text --params '{"text":"hello","txDelayMs":25}'
openclaw nodes invoke --node node-host --command cc1101.packet_rx_once --params '{"timeoutMs":5000}'

openclaw nodes invoke --node node-host --command system.run --params '{"command":["cc1101.info"]}'
```

## Gateway allowlist 주의

`cc1101.*` 직접 명령은 Gateway 정책에서 차단될 수 있습니다.
필요 시 아래 항목을 추가하세요.

```toml
[gateway.nodes]
allowCommands = [
  "cc1101.info",
  "cc1101.set_freq",
  "cc1101.tx",
  "cc1101.read_rssi",
  "cc1101.packet_get",
  "cc1101.packet_set",
  "cc1101.packet_tx_text",
  "cc1101.packet_rx_once",
]
```

## 법적 주의

무선 송신/재전송은 지역 법규를 준수해서 사용해야 합니다.
