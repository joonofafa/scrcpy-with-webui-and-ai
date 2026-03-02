# scrcpy 기술 스택

## 언어

| 언어 | 용도 |
|------|------|
| C (C11) | 클라이언트 코어, AI 에이전트, 웹 서버 |
| C++ | ImGui AI 패널 (ai_panel.cpp) |
| Java | Android 서버 (scrcpy-server.jar) |
| JavaScript (ES5) | 웹 UI (web_ui.h에 임베딩) |
| HTML/CSS | 웹 UI |
| Meson/Ninja | 빌드 시스템 |
| Bash | 릴리스 빌드 스크립트 |

## 빌드 시스템

- **Meson** + **Ninja** — C/C++ 클라이언트 빌드
- **Gradle** — Java 서버 빌드
- 정적 릴리스 빌드: `release/build_linux.sh x86_64`
- 증분 빌드: `ninja -C release/work/build-linux-x86_64`
- 빌드 옵션: `meson_options.txt` (`ai_panel`, `v4l2`, `usb`, `portable`, `static`)

## 핵심 라이브러리

### 원본 scrcpy

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| FFmpeg | libavformat ≥57.33, libavcodec ≥57.37 | H.264/H.265/AV1 디코딩, 오디오 디코딩, 녹화 |
| SDL2 | ≥2.0.5 | 윈도우, 렌더링, 입력, 오디오, 스레딩 |
| libusb-1.0 | (선택) | USB AOA 프로토콜 |
| libavdevice | (선택) | V4L2 가상 웹캠 |

### 추가 (AI/웹 기능)

| 라이브러리 | 버전 | 용도 | 경로 |
|-----------|------|------|------|
| Mongoose | 7.20 | HTTP/WebSocket 서버 | app/deps/mongoose/ |
| cJSON | — | JSON 파싱 (API 요청/응답, WebSocket 메시지) | app/deps/cjson/ |
| libcurl | 시스템 | OpenRouter/OpenAI API HTTP 클라이언트 | 시스템 패키지 |
| jmuxer.js | 2.0.5 | 브라우저 H.264 → MSE HW 디코딩 | CDN |

## 프로토콜 & 포맷

| 프로토콜 | 용도 |
|---------|------|
| ADB (Android Debug Bridge) | 기기 연결, 서버 푸시/실행, 소켓 터널 |
| H.264 Annex-B | 비디오 스트리밍 (start code 00 00 00 01) |
| WebSocket (RFC 6455) | 비디오 바이너리 스트림 + 컨트롤 JSON 메시지 |
| OpenAI Chat Completions API | AI 에이전트 LLM 호출 (OpenRouter 경유) |
| MSE (Media Source Extensions) | 브라우저 비디오 재생 |

## 아키텍처 패턴

- **Trait 기반 합성** — `sc_packet_sink`, `sc_frame_sink` 인터페이스 + `container_of` 매크로
- **Producer-Consumer 링버퍼** — 디먹서 스레드 → 웹 서버 스레드 (web_video_sink)
- **Function Calling** — LLM tool_calls → `sc_ai_tools_execute()` → 터치/키/스와이프 실행
- **싱글 스레드 이벤트 루프** — Mongoose mg_mgr_poll (HTTP + WebSocket + 비디오 브로드캐스트)

## 배포 환경

| 항목 | 값 |
|------|-----|
| OS | Ubuntu Linux (x86_64) |
| 서비스 | systemd (`scrcpy-web.service`) |
| 포트 | 8080 (HTTP + WebSocket) |
| 바인딩 | 0.0.0.0 (외부 접속 허용) |
| 대상 기기 | Samsung SM-F721N (Android 16), USB 연결 |

## 스레딩 모델 (클라이언트)

| 스레드 | 역할 |
|--------|------|
| Main | SDL 이벤트 루프 |
| Video Demuxer | 소켓 → H.264 패킷 파싱 → packet_sink (decoder + recorder + web_video_sink) |
| Audio Demuxer | 소켓 → 오디오 패킷 파싱 |
| Controller | 제어 메시지 큐 → 서버 전송 |
| Receiver | 서버 → 클라이언트 메시지 수신 |
| Web Server | Mongoose 이벤트 루프 (HTTP/WS 서빙 + 비디오 브로드캐스트) |
| AI Worker | LLM API 호출 + tool 실행 |
| AI Auto-play | 주기적 자동 스크린샷 → LLM 분석 → 조작 |

## 디렉토리 구조 (커스텀 부분)

```
app/src/ai/
├── ai_agent.c/h           # AI 에이전트 코어 (대화, LLM 호출, auto-play)
├── ai_agent_bridge.c/h    # C++ ↔ C 인터페이스
├── ai_frame_sink.c/h      # 디코딩된 프레임 → PNG 캡처
├── ai_panel.cpp/h         # ImGui 데스크톱 패널
├── ai_tools.c/h           # Function calling 도구 (터치, 키, 스와이프 등)
├── openrouter.c/h         # OpenRouter API 클라이언트
├── screenshot.c/h         # AVFrame → PNG 인코딩
├── web_server.c/h         # Mongoose HTTP/WebSocket 서버
├── web_video_sink.c/h     # sc_packet_sink → 링버퍼 → Annex-B 변환
└── web_ui.h               # 웹 UI (HTML/CSS/JS C 문자열)

app/deps/
├── cjson/                 # cJSON 라이브러리
└── mongoose/              # Mongoose 웹 서버 라이브러리
```
