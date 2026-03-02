# scrcpy 프로젝트 Context

## 프로젝트 개요

scrcpy(Screen Copy) 3.3.4 기반 포크. 원본 기능(Android 미러링/제어) 위에 **AI 에이전트** + **웹 릴레이 서버** 기능을 추가했다.

## 커스텀 기능 (app/src/ai/)

### 1. AI Vision Agent

LLM(OpenRouter API)이 Android 화면을 보고 자율적으로 조작하는 시스템.

- **ai_agent.c/h** — 에이전트 코어. 대화 히스토리 관리, worker 스레드에서 LLM 호출
- **ai_frame_sink.c/h** — `sc_frame_sink` 구현. 디코딩된 비디오 프레임을 PNG로 캡처
- **screenshot.c/h** — FFmpeg AVFrame → PNG 인코딩 (libavcodec png encoder)
- **openrouter.c/h** — OpenRouter/OpenAI 호환 Chat Completions API 클라이언트 (libcurl)
- **ai_tools.c/h** — Function calling 도구 정의 및 실행 (터치, 스와이프, 키 입력, 텍스트 입력, 대기 등)
- **ai_agent_bridge.c/h** — ImGui 패널 ↔ AI 에이전트 브릿지 (C++ ↔ C 인터페이스)
- **ai_panel.cpp/h** — ImGui 기반 데스크톱 AI 제어 패널 (채팅/학습/자동 탭)

CLI 옵션: `--ai-panel`, `--ai-api-key <key>`

### 2. Web Relay Server (헤드리스 원격 제어)

리눅스 서버에서 scrcpy를 헤드리스 데몬으로 실행, 웹 브라우저로 Android 화면 실시간 중계 + 터치/키 원격 조작.

- **web_server.c/h** — Mongoose 7.20 기반 HTTP/WebSocket 서버
  - `GET /` — 웹 UI HTML 서빙
  - `GET /ws/video` — H.264 비디오 스트림 (바이너리 WebSocket)
  - `GET /ws/control` — 터치/키 입력 수신 (JSON WebSocket)
  - 디바이스 회전 시 자동 해상도 변경 브로드캐스트
- **web_video_sink.c/h** — `sc_packet_sink` 구현. 디먹서 H.264 패킷 → 링버퍼 큐
  - `ensure_annexb()` — 모든 패킷을 Annex-B 포맷(00 00 00 01 start code)으로 변환
  - merged 패킷(config+키프레임) 분리 처리
- **web_ui.h** — 웹 UI HTML/CSS/JS를 C 문자열 상수로 임베딩
  - 좌우 2패널: 좌측=Remote(비디오+터치), 우측=AI 패널(채팅/학습/자동)
  - jmuxer.js로 H.264 → MSE 브라우저 HW 디코딩 (재인코딩 없음)
  - 로딩 스피너 (video timeupdate 이벤트로 실제 재생 시 숨김)
  - 터치 좌표 변환: object-fit:contain 보정 → video.videoWidth/Height 기반 매핑

CLI 옵션: `--ai-web-port <port>`

실행 예시: `scrcpy --no-window --no-audio --ai-panel --ai-web-port 8080 -s <device>`

### 3. systemd 서비스

`/etc/systemd/system/scrcpy-web.service` — 부팅 시 자동 시작, 실패 시 자동 재시작

## 빌드

```bash
# 릴리스 빌드 (정적 링크)
release/build_linux.sh x86_64

# 증분 빌드
ninja -C release/work/build-linux-x86_64

# 배포
sudo systemctl stop scrcpy-web
cp release/work/build-linux-x86_64/app/scrcpy release/work/build-linux-x86_64/dist/scrcpy
sudo systemctl start scrcpy-web
```

빌드 옵션: `meson_options.txt`의 `ai_panel` (default: false)

## 주요 설계 결정

- **MAX_SINKS=3**: `app/src/trait/packet_source.h`에서 2→3. 디먹서에 decoder + recorder + web_video_sink 연결
- **Mongoose c->data[0]**: WebSocket 타입 마킹('V'=video, 'C'=control)은 반드시 `mg_ws_upgrade()` 호출 **전에** 설정. MG_EV_WS_OPEN이 동기적으로 발생하기 때문
- **Annex-B 변환은 C에서**: JS가 아닌 서버 측 `ensure_annexb()`에서 처리. packet_merger가 만드는 [config][raw IDR] merged 패킷의 start code 삽입을 정확히 처리
- **바인딩 0.0.0.0**: 외부 네트워크 접속 허용

## 의존성 (추가분)

| 라이브러리 | 용도 | 경로 |
|-----------|------|------|
| Mongoose 7.20 | HTTP/WebSocket 서버 | app/deps/mongoose/ |
| cJSON | JSON 파싱 | app/deps/cjson/ |
| libcurl | OpenRouter API 호출 | 시스템 패키지 |
| jmuxer.js 2.0.5 | 브라우저 H.264→MSE | CDN (web_ui.h에서 로드) |
