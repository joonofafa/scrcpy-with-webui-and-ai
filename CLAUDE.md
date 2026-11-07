# scrcpy-ai 프로젝트 컨텍스트

scrcpy v3.3.4 포크 + **웹 원격 제어** 기능. 헤드리스 서버에서 Android 기기(실기기/Redroid)를
브라우저로 미러링·제어하는 것이 현재 목적.

**주의**: README.md와 `.claude/claude.md`, `.claude/skill.md`는 AI 에이전트(agent/, clip/,
llm/, pipeline/ 모듈) 시절의 설명이 남아 있다. 커밋 620fd932("Refactor to pure web remote")로
해당 모듈은 **삭제됨**. 현재 Python은 인증 + WS 프록시 + 정적 대시보드만 담당.
(ai-venv에 chromadb/open_clip이 설치되어 있으나 현 코드에서 미사용.)

## 아키텍처 (3 컴포넌트)

```
브라우저 → Apache(443, SSL) 또는 FastAPI(8080) 직접 접속
  ├─ /ws/video, /ws/control   → C 백엔드 Mongoose 서버 (18080, Z-Flip4 실기기)
  ├─ /ws2/video, /ws2/control → 18081 (Redroid-iSharing, localhost:5603) — 온디맨드*
  ├─ /ws3/video, /ws3/control → 18082 (Redroid-Kakao, localhost:5601)
  ├─ /ws4/video, /ws4/control → 18083 (LG Velvet 실기기)
  └─ / (나머지)               → Python FastAPI (8080): TOTP 로그인, 정적 UI
```

\* `scrcpy-isharing`은 **보는 사람이 있을 때만 실행**: main.py가 `/ws2` 접속 시 `sudo systemctl start`,
  시청자 0명으로 `ISHARING_IDLE_STOP`(300s) 지나면 stop(scrcpy-ai 기동 시에도 예약). 지도가 계속
  움직여 컨테이너 인코딩 CPU를 상시 먹었기 때문(2026-11-07, Bill 결정, iSharing-Agent 동의 —
  isharing-agent는 adb만 쓰고 18081 미사용). 꺼져 있으면 MCP `isharing` 기기·`/internal`도 응답 없음.

- **C 클라이언트** `app/src/` — 원본 scrcpy + `app/src/web/`(webroute 기능)
  - `web_server.c` — Mongoose 7.20 HTTP/WS 서버. `/internal/{screenshot,info,click,long_press,swipe,key,text}`, `/ws/{video,control}`
  - `web_video_sink.c` — packet_sink → Annex-B 변환 + 키프레임 캐시(SPS/PPS 변경 시 무효화)
  - `web_frame_sink.c` — 스크린샷용 디코딩 프레임 버퍼
  - 역할 분리 원칙: **C는 디바이스 I/O만, 나머지 로직은 전부 Python 쪽**
- **Java 서버** `server/` — Android로 push되는 scrcpy-server (업스트림 그대로)
- **Python** `app/python/scrcpy_ai/` — `main.py`(FastAPI 앱 + WS 프록시), `auth.py`(TOTP +
  세션 지속화), `web/routes.py`(`/auth/login`, `/auth/setup`), `static/index.html`(대시보드,
  jmuxer 2.1.0 H.264→MSE), `mcp_server.py`(MCP 서버, 아래 참고)

**운영 경로 주의**: 모든 systemd 유닛(scrcpy-web/isharing/kakao/ai)은
`/mnt/extHdd/McpTools/scrcpy-with-webui-and-ai`(이 저장소)에서 실행된다.
`~/gitHub/scrcpy-with-webui-and-ai`는 이 저장소를 가리키는 **심볼릭 링크**(2026-11-02, 예전
별도 클론을 정리함 — 백업은 `.temp/stale-clone-backup-20261102/`). 다시 클론하지 말 것.

## 게임 에이전트 (웹 대시보드 좌측 패널)

VLM(z.ai `glm-5.3-flash`)이 스크린샷을 보고 툴 호출로 게임을 조작하는 에이전트. 2026-10-15에
`~/gitHub/zaiGameTelegramBridge`에서 **이 패키지로 이관**됨 (bridge 저장소는 동결 — 수정 금지,
Telegram 봇 경로는 토큰 폐기로 사망).

- `agent_api.py` — 웹 어댑터: `/ws/agent/chat`(main.py에서 `_ws_authorized` 가드),
  `/agent/{skills,icon,skill,history,status,stop,auto}`, 세션 녹화
- `game_agent.py` — 엔진: `GameAgentSession`(턴 루프·컨텍스트 압축), `run_session_tool()`
  (도구 실행), 스킬/게임지식 저장. C `/internal/*`를 HTTP로 직접 호출(MCP 미사용).
  click/long_press/swipe/key_press는 `GAME_AGENT_SHOT_DELAY`(기본 0.8s) 뒤 스크린샷을
  자동 첨부하고, 결과 문구에 모델이 준 **이미지 좌표**를 적는다(별도 screenshot 왕복 제거).
- 로컬 Jev 모델(`~/gitHub/decisionTestAgent`) 의존은 **완전히 제거됨**: 유료 소비 하드게이트는
  2026-11-02, 루프 가드 막힘 판정(G3)은 2026-11-07(입력이 고정 문장이라 판정이 늘 같았음).
  루프 가드는 같은/근접 좌표 3회 반복 시 경고 문구만 붙인다. 다시 넣지 말 것(Bill 결정).
- 데이터: `~/.scrcpy_ai/db/{gameSkills,gameKnowledge,recordings}` (`GAME_AGENT_WORKING_DIR`로 변경 가능)
- 설정: `GAME_AGENT_*` env 네임스페이스만 읽음. API 키는 `~/.apikeys/zai.gameAgent`
- 수정 후 반영: `sudo systemctl restart scrcpy-ai`

## MCP 서버 (화면 조회 + 제어)

`app/python/scrcpy_ai/mcp_server.py` — stdio MCP 서버. C 백엔드 `/internal/*` HTTP API를
래핑해 AI 에이전트가 기기 화면을 보고 제어하게 함. 실행:
`ai-venv/bin/python app/python/scrcpy_ai/mcp_server.py` (`.mcp.json`에 등록 완료).

- 도구: `screenshot`(JPEG + width/height 텍스트 반환, 500 `encode failed` 시 0.8s 간격 최대 3회
  재시도), `screen_info`, `click`, `long_press`, `swipe`, `key_press`(대소문자 무관, 숫자 keycode
  스키마 `str|int`), `input_text`, `list_devices`
- 디바이스: `phone`(18080, 기본값), `isharing`(18081), `kakao`(18082)
- **좌표계는 스크린샷 픽셀 좌표** — C `clamp_coords()`가 프레임 좌표로 자동 매핑/클램프.
  `screenshot` 결과로 보고 클릭 좌표를 그대로 사용하면 됨.
- MCP SDK는 **2.x**: `from mcp.server.mcpserver import MCPServer` (1.x의 FastMCP명이 아니며,
  이미지 content 키는 `mimeType` camelCase).

## 빌드/테스트/실행

```bash
# C 증분 빌드 (정석 경로 — release, -Dwebroute=true)
ninja -C release/work/build-linux-x86_64

# 새로 설정할 때 (release/build_linux.sh는 구버전 옵션 -Dai_panel이 남아 있어 그대로 쓰지 말 것)
DEPS=$PWD/app/deps/work/install/linux-native-static
meson setup <builddir> --pkg-config-path="$DEPS/lib/pkgconfig" \
  -Dc_args="-I$DEPS/include" -Dc_link_args="-L$DEPS/lib" \
  --buildtype=release --strip -Db_lto=true -Dcompile_server=false \
  -Dportable=true -Dstatic=true -Dwebroute=true
# 링크에 libx11/xcb/drm/xdmcp/xau/udev 의 -dev 패키지가 필요(없으면 "cannot find -lX11").
# 정적 의존성 .pc 파일은 절대경로를 품는다 — 저장소를 옮기면 prefix를 다시 고쳐야 함.

# C 유닛테스트 (buildtype=debug일 때만 빌드됨 — release 빌드 디렉토리에선 없음)
meson test -C build

# Java 서버: 루트 ./gradlew 또는 server/build_without_gradle.sh

# Python 실행 (ai-venv는 저장소 내 gitignored venv)
ai-venv/bin/python -m scrcpy_ai.main --port 8080 --scrcpy-port 18080

# 배포: dist로 복사 후 서비스 재시작
cp release/work/build-linux-x86_64/app/scrcpy release/work/build-linux-x86_64/dist/scrcpy
sudo systemctl restart scrcpy-web   # scrcpy-isharing / scrcpy-kakao / scrcpy-ai 동일
```

systemd 유닛: `scrcpy-web`(18080), `scrcpy-isharing`(18081), `scrcpy-kakao`(18082),
`scrcpy-web2`(18083, Velvet),
`scrcpy-ai`(FastAPI 8080). Mongoose 로그는 env `SC_WEB_LOG_LEVEL`(0~4, 기본 1=error; 3=debug는
패킷마다 한 줄이라 journal이 넘침). 실행 시 `--no-window --no-audio --webroute <port>
--video-bit-rate=4M --max-size=1280 -s <serial>` 형태.

## 주요 규칙/주의사항

- **webroute 코드는 조건부 컴파일**: `meson_options.txt`의 `webroute`(기본 false) →
  `#ifdef HAVE_WEB_ROUTE`. webroute 관련 수정은 `-Dwebroute=true` 빌드로 검증해야 함.
  루트 `build/`는 webroute 없는 로컬 debug 디렉토리.
- **WebSocket은 AuthMiddleware를 우회함**(Starlette BaseHTTPMiddleware는 scope=="http"만
  처리). 새 WS 엔드포인트는 반드시 `main.py`의 `_ws_authorized()`를 직접 호출해 인증.
- **세션 TTL 이중 유지**: `auth.py`의 `SESSION_TTL`과 `web/routes.py`의 cookie `max_age`
  (2시간)는 수동으로 일치시켜야 함.
- **디바이스 슬롯 추가 시 4곳 수정**: `main.py`(WS 라우트, `config.scrcpy_port + N`),
  `static/index.html`의 DEVICES 배열(`kind: 'phone'|'redroid'` 필수 — 우하단 빨강/초록
  플로팅 버튼의 폰⇄에이전트 뷰 토글이 이 값으로 카드를 필터링), systemd 유닛(포트/시리얼),
  **Apache `jhsrc.conf`의 `/wsN/*` ProxyPass 블록**(누락 시 외부 접속만 조용히 실패).
  `sites-enabled/jhsrc.conf`는 `sites-available`로의 **심볼릭 링크여야 함** — 일반 파일로
  바뀌어 있으면 `sites-available` 수정이 적용되지 않는다(2026-10-17에 실제로 발생).
  `ls -l /etc/apache2/sites-enabled/jhsrc.conf`로 확인.
- **웹 키보드 입력 경로**: `-K`(UHID) 유닛(Flip4·Velvet)은 제어 WS가 열릴 때 `{"type":"caps",
  "uhidKeyboard":true}`를 보내고, 프론트는 키를 USB HID 사용 코드로 `{type:"hidkey"}`에 실어
  보낸다 → `web_server.c`가 scrcpy가 만든 UHID 키보드(ID 1)로 리포트를 전송. 폰이 물리
  키보드로 인식하므로 **한글은 폰의 키보드 앱이 조합**(폰 설정의 물리 키보드 언어 전환 단축키
  사용). scrcpy의 텍스트 주입(`INJECT_TEXT`)은 가상 키맵을 거쳐 한글을 버리므로 쓰지 말 것.
  UHID 키 처리기(`keyboard_uhid.c`)는 메인 스레드 전용이라 웹 스레드는 하위 생성기
  `sc_hid_keyboard_generate_input_from_key()`를 직접 쓴다. 키가 눌린 채 남지 않도록 KB 끄기·
  창 포커스 이탈·제어 WS 종료 시 `hidreset`으로 전부 뗀다.
- **Redroid(비UHID) 한글 입력**: caps의 `"paste":true`이면 KB ON 시 숨은 textarea(`imeSink`)에
  포커스를 둬서 PC IME가 조합하게 하고, 확정된 글자를 250ms 단위로 묶어 `{type:"paste"}` →
  scrcpy SET_CLIPBOARD(paste=true)로 붙여넣음. 묶는 이유: 연속 붙여넣기가 기기 클립보드에서
  경합함. 다른 키를 보내기 전에 대기 중인 텍스트를 먼저 flush해 순서를 지킴. Android 13은
  붙여넣을 때마다 클립보드 미리보기 오버레이와 토스트가 뜨는데, 이는 정상 동작.
- **Redroid 대상 scrcpy 유닛**: `-K`(UHID) 금지 — redroid에서 크래시, **`--keyboard=sdk`** 사용.
  adb-tcpip 대상이므로 `ExecStartPre=<adb> connect localhost:<port>` 필요.
- **Redroid 미러 잔상(유령 UI/글자 누락)은 알려진 수용된 이슈 — 재조사 금지**: 컨테이너가
  `gpu_mode=guest`(SwiftShader)라 인코더 입력 버퍼에 damage-region 잔상이 남는 것.
  host 모드 전환은 redroid #93(AMD APU Mesa 버그)으로 봇이 읽는 온디바이스 렌더를 깨뜨려
  기각됨(Bill 결정). 판별법: `adb screencap`은 깨끗한데 스트림만 깨지면 이 이슈.
- **비디오 파이프라인 불변식** (하나라도 깨면 화면 깨짐 재발):
  - scrcpy `--max-fps=30` ↔ `index.html` jmuxer `fps: 30` 일치 필수(불일치 → 타임라인
    드리프트 → 지연 누적).
  - 지연 보정은 **playbackRate**(1.12x)로만. `video.currentTime` 시크 금지 — 중간 GOP
    시크는 다음 키프레임까지 매크로블록 깨짐.
  - `patchSpsLevel()`은 일부 기기 인코더의 잘못된 SPS level_idc(예: LG가 1280px 프레임에
    Level 1.0 기록)를 4.1로 교정 — 제거하면 해당 기기에서 브라우저 디코드 깨짐.
  - 로딩 오버레이는 첫 디코드 가능 프레임(`loadeddata`/buffered)에서 내려감 — `timeupdate`
    만으로 되돌리면 정지 화면 기기가 "Loading…"에 영원히 걸림.
  - 스트리밍은 **현재 보기(phone/redroid)의 카드만** 연결(`connect()/disconnect()`), 보기 전환 시
    나머지는 WS를 닫음. 브라우저 디코드·대역폭만 줄어들 뿐, 기기 쪽 인코딩은 scrcpy 유닛이 떠 있는
    한 계속됨(C 백엔드에 시청자 유무 게이트 없음).
  - 회전 처리: 기기 회전 시 scrcpy가 새 해상도로 재인코딩(새 SPS). jmuxer는 **첫 SPS로만**
    init segment를 만들므로 프론트가 SPS 변경을 감지해 jmuxer를 재생성(`findSps`). CSS 회전
    보정은 없음(예전 "Chrome 회전 보정"은 C 메타데이터가 시작 크기에 고정돼 생긴 오진이었음).
    C 메타데이터 크기는 디코드된 프레임 크기(`web_get_video_size`)를 따름. 터치 w/h는
    `video.videoWidth/Height` — 서버는 현재 비디오 크기와 다른 screen_size 터치를 버림.
- C 스타일: 업스트림 scrcpy 규칙 — `sc_` 접두사, snake_case, internal 함수 static,
  `-Wmissing-prototypes`, C11. app/meson.build에 소스 파일 명시 필요.
- 상태 저장: `~/.scrcpy_ai/db/`(TOTP 시크릿, sessions.json — chmod 600).
- 인증 우회 판정은 `auth.is_internal_request`(client host / X-Forwarded-For / Host 헤더).
  Apache 리버스 프록시 환경 가정.

## 디렉토리 구조

```
app/src/        C 클라이언트 (원본 + web/ = webroute 추가분)
app/src/web/    Mongoose 서버, 비디오/프레임 싱크, 입력 주입, JPEG 스크린샷
app/deps/       mongoose/, cjson/ (webroute 의존, 저장소 포함)
app/tests/      C 유닛테스트
app/python/     scrcpy_ai 패키지 (pyproject.toml)
server/         Android용 Java 서버 (gradle)
release/        멀티플랫폼 빌드/릴리스 스크립트, work/build-linux-x86_64 = 실제 사용 빌드
ai-venv/        Python 가상환경 (gitignored, scrcpy-ai 서비스가 사용)
doc/, FAQ.md    업스트림 scrcpy 문서
.claude/        이전 세션의 상세 컨텍스트(claude.md, skill.md) — 일부 내용이 구버전
```
