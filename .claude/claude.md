# scrcpy 프로젝트 Context

## 프로젝트 개요

scrcpy(Screen Copy) 3.3.4 기반 포크. 원본 기능(Android 미러링/제어) 위에 **웹 라우트 서버**(C) + **AI 프론트엔드**(Python) 기능을 추가했다.

**역할 분리 원칙**: C는 디바이스 I/O만 (비디오 스트리밍 + 터치/키 주입 + 스크린샷), AI 로직은 전부 Python.

## C 백엔드: Web Route Server (app/src/web/, 포트 18080)

`--webroute <port>` 옵션으로 활성화. 디바이스 화면/제어를 HTTP/WebSocket API로 노출.

- **web_server.c/h** — Mongoose 7.20 HTTP/WebSocket 서버
  - `GET /ws/video` — H.264 비디오 스트림 (바이너리 WebSocket)
  - `GET /ws/control` — 터치/키 입력 수신 (JSON WebSocket)
  - `GET /internal/screenshot` — JPEG 스크린샷 + 해상도 헤더
  - `POST /internal/click` — 터치 주입 (DOWN/MOVE/UP)
  - `POST /internal/long_press` — 롱프레스
  - `POST /internal/swipe` — 스와이프
  - `POST /internal/key` — 키코드 주입
  - `POST /internal/text` — 텍스트 입력
  - `GET /internal/info` — 화면 해상도 정보
- **web_video_sink.c/h** — `sc_packet_sink` 구현. 디먹서 H.264 패킷 → 링버퍼 큐
  - Annex-B 변환, 키프레임 캐싱, SPS/PPS 변경 감지
- **web_frame_sink.c/h** — `sc_frame_sink` 구현. 디코딩된 프레임 버퍼 (스크린샷용)
- **web_tools.c/h** — 터치/키/스와이프 주입 (scrcpy controller 경유)
- **screenshot.c/h** — AVFrame → JPEG 인코딩 (libswscale + MJPEG)

CLI 옵션: `--webroute <port>`

## Python 프론트엔드 (app/python/, 포트 8080)

FastAPI 기반. AI 에이전트 + 웹 UI + C 백엔드 /internal/* API 래핑.

- **main.py** — FastAPI 앱 진입점 + WebSocket 프록시 (C 백엔드 WS 중계, Apache 없이 직접 접근 가능)
- **web/routes.py** — API 라우트 (AI 에이전트, 게임 규칙, 학습, CLIP 등)
- **static/index.html** — 웹 UI (jmuxer 2.1.0 H.264→MSE 디코딩)
- **agent/agent.py** — AI 에이전트 (VLM 화면분석, LLM 판단, tool 실행)
- **clip/matcher.py** — CLIP 임베딩 기반 화면 매칭
- **device/client.py** — C 백엔드 /internal/* API 호출 클라이언트
- **llm/openrouter.py** — OpenRouter LLM API 클라이언트
- **pipeline/recorder.py** — Record/Train/Play 파이프라인

## 배포 아키텍처

```
[방법 1] Apache 리버스 프록시
브라우저 → Apache (443/SSL)
  ├─ /ws/video, /ws/control → C 백엔드 (ws://127.0.0.1:18080)
  └─ / (나머지) → Python FastAPI (http://127.0.0.1:8080)

[방법 2] Python 직접 접근 (Apache 없이)
브라우저 → Python FastAPI (8080)
  ├─ /ws/video, /ws/control → WebSocket 프록시 → C 백엔드 (ws://127.0.0.1:18080)
  └─ / (나머지) → Python 직접 처리
```

- systemd 서비스: `/etc/systemd/system/scrcpy-web.service`
- 실행: `scrcpy --no-window --no-audio --webroute 18080 --video-bit-rate=4M --video-codec-options=i-frame-interval=2 --max-size=1280 -s <device>`

## 빌드

```bash
# 증분 빌드
ninja -C release/work/build-linux-x86_64

# 배포
sudo systemctl stop scrcpy-web
cp release/work/build-linux-x86_64/app/scrcpy release/work/build-linux-x86_64/dist/scrcpy
sudo systemctl start scrcpy-web
```

빌드 옵션: `meson_options.txt`의 `webroute` (default: false)

## 주요 설계 결정

- **MAX_SINKS=3**: 디먹서에 decoder + recorder + web_video_sink 연결
- **Mongoose c->data[0]**: WebSocket 타입 마킹은 `mg_ws_upgrade()` 호출 **전에** 설정
- **Annex-B 변환은 C에서**: 서버 측 `ensure_annexb()`에서 처리
- **키프레임 캐시 무효화**: SPS/PPS 변경 시 캐시 삭제 (앱 전환 안전성)
- **C ↔ Python 분리**: C는 디바이스 I/O만, AI 로직은 Python만

## 의존성 (C 추가분)

| 라이브러리 | 용도 | 경로 |
|-----------|------|------|
| Mongoose 7.20 | HTTP/WebSocket 서버 | app/deps/mongoose/ |
| cJSON | JSON 파싱 | app/deps/cjson/ |
| libswscale | 스크린샷 이미지 스케일링 | 시스템 패키지 |
