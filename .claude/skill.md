# scrcpy 기술 스택

## 언어

| 언어 | 용도 |
|------|------|
| C (C11) | 클라이언트 코어, AI 에이전트, 웹 서버 (C 백엔드) |
| Python 3.12 | 웹 프론트엔드 (FastAPI), AI 에이전트 래퍼, CLIP 매칭 |
| Java | Android 서버 (scrcpy-server.jar) |
| JavaScript (ES5) | 웹 UI (jmuxer, WebSocket, 터치 입력) |
| HTML/CSS | 웹 UI |
| Meson/Ninja | 빌드 시스템 |
| Bash | 릴리스 빌드 스크립트 |

## 빌드 시스템

- **Meson** + **Ninja** — C 클라이언트 빌드
- **Gradle** — Java 서버 빌드
- **pip** — Python 패키지 (`app/python/pyproject.toml`)
- 증분 빌드: `ninja -C release/work/build-linux-x86_64`
- 빌드 옵션: `meson_options.txt` (`webroute`, `v4l2`, `usb`, `portable`, `static`)

## 핵심 라이브러리

### 원본 scrcpy

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| FFmpeg | libavformat ≥57.33, libavcodec ≥57.37 | H.264/H.265/AV1 디코딩, 오디오 디코딩, 녹화 |
| SDL2 | ≥2.0.5 | 윈도우, 렌더링, 입력, 오디오, 스레딩 |
| libusb-1.0 | (선택) | USB AOA 프로토콜 |
| libavdevice | (선택) | V4L2 가상 웹캠 |

### 추가 (웹 라우트) — C

| 라이브러리 | 버전 | 용도 | 경로 |
|-----------|------|------|------|
| Mongoose | 7.20 | HTTP/WebSocket 서버 | app/deps/mongoose/ |
| cJSON | — | JSON 파싱 (API 요청/응답, WebSocket 메시지) | app/deps/cjson/ |
| libswscale | 시스템 | 스크린샷 이미지 스케일링 | 시스템 패키지 |

### 추가 (프론트엔드) — Python

| 라이브러리 | 용도 |
|-----------|------|
| FastAPI | 웹 프레임워크 (REST API, 정적 파일 서빙) |
| uvicorn | ASGI 서버 |
| httpx | C 백엔드 통신 HTTP 클라이언트 |
| websockets | WebSocket 프록시 (C 백엔드 WS 중계) |

### 추가 (브라우저)

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| jmuxer.js | 2.1.0 | H.264 → MSE 브라우저 HW 디코딩 |

## 프로토콜 & 포맷

| 프로토콜 | 용도 |
|---------|------|
| ADB (Android Debug Bridge) | 기기 연결, 서버 푸시/실행, 소켓 터널 |
| H.264 Annex-B | 비디오 스트리밍 (start code 00 00 00 01) |
| WebSocket (RFC 6455) | 비디오 바이너리 스트림 + 컨트롤 JSON 메시지 |
| OpenAI Chat Completions API | AI 에이전트 LLM 호출 (OpenRouter 경유) |
| MSE (Media Source Extensions) | 브라우저 비디오 재생 |
| CLIP Embedding API | 화면 유사도 매칭 (CLIP 서버 경유) |

## 아키텍처 패턴

- **Trait 기반 합성** — `sc_packet_sink`, `sc_frame_sink` 인터페이스 + `container_of` 매크로
- **Producer-Consumer 링버퍼** — 디먹서 스레드 → 웹 서버 스레드 (web_video_sink)
- **키프레임 캐싱** — 마지막 IDR 프레임 캐시, 새 WS 클라이언트 접속 시 즉시 전송
- **SPS/PPS 변경 감지** — config 변경 시 키프레임 캐시 무효화 (앱 전환 안전성)
- **C/Python 역할 분리** — C는 디바이스 I/O만 (/internal/* API), AI는 Python만
- **싱글 스레드 이벤트 루프** — Mongoose mg_mgr_poll (HTTP + WebSocket + 비디오 브로드캐스트)
- **리버스 프록시 분리** — Apache가 WebSocket은 C 백엔드로, HTTP는 Python으로 라우팅

## 배포 환경

| 항목 | 값 |
|------|-----|
| OS | Ubuntu Linux (x86_64) |
| 서비스 | systemd (`scrcpy-web.service`) |
| 리버스 프록시 | Apache2 (mod_proxy, mod_proxy_wstunnel, SSL) |
| C 백엔드 포트 | 18080 (WebSocket: 비디오 스트림 + 컨트롤) |
| Python 프론트엔드 포트 | 8080 (HTTP: 웹 UI + REST API) |
| 외부 포트 | 443 (SSL, Apache) |
| 대상 기기 | Samsung SM-F721N (Android 16), USB 연결 |

## 스레딩 모델 (클라이언트)

| 스레드 | 역할 |
|--------|------|
| Main | SDL 이벤트 루프 (헤드리스 시 최소화) |
| Video Demuxer | 소켓 → H.264 패킷 파싱 → packet_sink (decoder + recorder + web_video_sink) |
| Audio Demuxer | 소켓 → 오디오 패킷 파싱 |
| Controller | 제어 메시지 큐 → 서버 전송 |
| Receiver | 서버 → 클라이언트 메시지 수신 |
| Web Server | Mongoose 이벤트 루프 (HTTP/WS 서빙 + 비디오 브로드캐스트) |

## 디렉토리 구조 (커스텀 부분)

```
app/src/web/
├── web_server.c/h         # Mongoose HTTP/WebSocket 서버 (/internal/* + /ws/*)
├── web_video_sink.c/h     # sc_packet_sink → 링버퍼 → Annex-B + 키프레임 캐시
├── web_frame_sink.c/h     # 디코딩된 프레임 → 버퍼 (스크린샷용)
├── web_tools.c/h          # 터치/키/스와이프 주입
└── screenshot.c/h         # AVFrame → JPEG 인코딩

app/data/
└── clip_server.py         # CLIP 임베딩 서버 (독립 실행)

app/python/scrcpy_ai/
├── main.py                # FastAPI 앱 진입점 + WebSocket 프록시
├── config.py              # 설정
├── static/index.html      # 웹 UI (jmuxer 2.1.0)
├── web/routes.py          # API 라우트
├── agent/agent.py         # AI 에이전트 래퍼
├── clip/matcher.py        # CLIP 임베딩 매칭
├── device/
│   ├── client.py          # C 백엔드 통신
│   ├── tool_executor.py   # 도구 실행기
│   └── tools.py           # 도구 정의
├── llm/openrouter.py      # LLM API 클라이언트
└── pipeline/recorder.py   # Record/Train/Play 파이프라인

app/deps/
├── cjson/                 # cJSON 라이브러리
└── mongoose/              # Mongoose 웹 서버 라이브러리
```
