"""Web-UI agent panel: Telegram 없이 game_agent(GameAgentSession)를 웹에서 쓴다.

- WS  /ws/agent/chat  — 채팅. 클라이언트 → {type:"user", text}, 서버 →
  {type:"tool", name, desc} / {type:"reply", text} / {type:"error", text}
- GET /agent/skills   — 포그라운드 앱 패키지 감지 → 해당 스킬 목록(JSON)
- GET /agent/skill/{filename} — 스킬 본문(markdown)

주의(2026-10-06): zai-game-telegram-bridge 저장소의 game_agent.py를 import한다.
GAME_AGENT_* env 네임스페이스 분리 규칙(공유 env 오염 차단)은 그대로 적용된다.
"""

import asyncio
import json
import logging
import os
import re
import subprocess
import time

from fastapi import APIRouter, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse

logger = logging.getLogger(__name__)

# --- game_agent import -------------------------------------------------------
# A broken agent must not take down the web remote, so degrade to None.
try:
    from scrcpy_ai import game_agent
except Exception as e:  # pragma: no cover
    game_agent = None
    logger.warning("game_agent import failed: %s", e)

router = APIRouter(prefix="/agent")

# --- app icon/label cache ---------------------------------------------------
# 기기 내에서 app_process로 아이콘을 렌더링해 PNG로 캐시한다.
# (적응형 아이콘 XML + 현지 언어 라벨까지 기기가 알아서 처리)
# 캐시: ~/.scrcpy_ai/db/icons/{package}.png (아이콘), {package}.label (라벨)
_ICON_DIR = os.path.expanduser("~/.scrcpy_ai/db/icons")
_ICON_TTL = 30 * 86400  # 재추출 주기
_DEX_LOCAL = "/data/local/tmp/icondump.dex"
_DEX_BUNDLED = os.path.join(os.path.dirname(__file__), "icondump.dex")
# `pkg` is interpolated into `adb shell` strings, which the device's /bin/sh
# parses — only an Android package name shape may get through.
_PKG_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")


def _adb_path():
    return game_agent._find_adb() if game_agent else "adb"


def _extract_icon(pkg: str):
    """app_process 아이콘 덤프 → PNG 캐시. 성공 시 (png_path, label)."""
    if not pkg or not _PKG_RE.match(pkg):
        return None, None
    png = os.path.join(_ICON_DIR, f"{pkg}.png")
    lbl = os.path.join(_ICON_DIR, f"{pkg}.label")
    if os.path.exists(png) and os.path.getsize(png) > 0 and \
            time.time() - os.path.getmtime(png) < _ICON_TTL:
        label = None
        try:
            with open(lbl, encoding="utf-8") as f:
                label = f.read().strip()
        except OSError:
            pass
        return png, label
    adb = _adb_path()
    serial = getattr(game_agent, "ADB_SERIAL", "R3CTA0SCHGM")
    remote = f"/data/local/tmp/icon_{pkg}.png"
    try:
        # dex 최초 1회 push
        subprocess.run([adb, "-s", serial, "shell",
                        f"test -f {_DEX_LOCAL} || echo missing"],
                       capture_output=True, text=True, timeout=10)
        # (매번 stat하는 대신 push --sync 성능 저하를 피하려 단순 재push는 생략:
        #  없으면 아래 app_process가 실패하므로 그때 push)
        env = ("ANDROID_DATA=/data ANDROID_ROOT=/system "
               f"CLASSPATH={_DEX_LOCAL}")
        r = subprocess.run(
            [adb, "-s", serial, "shell",
             f"{env} app_process / com.jhsoft.icondump.IconDump "
             f"{pkg} {remote}"],
            capture_output=True, text=True, timeout=30)
        out = (r.stdout or "").strip()
        if not out.startswith("OK") and "icondump" in (r.stderr or ""):
            return None, None
        if not out.startswith("OK"):
            # dex 미존재 가능 → push 후 재시도 1회
            subprocess.run(
                [adb, "-s", serial, "push", _DEX_BUNDLED, _DEX_LOCAL],
                capture_output=True, timeout=30)
            r = subprocess.run(
                [adb, "-s", serial, "shell",
                 f"{env} app_process / com.jhsoft.icondump.IconDump "
                 f"{pkg} {remote}"],
                capture_output=True, text=True, timeout=30)
            out = (r.stdout or "").strip()
        if not out.startswith("OK"):
            logger.warning("icondump failed for %s: %s", pkg,
                           out or r.stderr)
            return None, None
        # "OK\t<path>\t<size>\t<label>"
        label = out.split("\t")[-1].strip() if "\t" in out else pkg
        os.makedirs(_ICON_DIR, exist_ok=True)
        tmp_png = png + ".tmp"
        pr = subprocess.run([adb, "-s", serial, "pull", remote, tmp_png],
                            capture_output=True, timeout=30)
        if pr.returncode != 0 or not os.path.isfile(tmp_png) or \
                os.path.getsize(tmp_png) == 0:
            logger.warning("icon pull failed for %s", pkg)
            try:
                os.unlink(tmp_png)
            except OSError:
                pass
            return None, None
        os.replace(tmp_png, png)
        with open(lbl, "w", encoding="utf-8") as f:
            f.write(label)
        subprocess.run([adb, "-s", serial, "shell", f"rm -f {remote}"],
                       capture_output=True, timeout=10)
        return png, label
    except Exception as e:
        logger.warning("icon extract failed for %s: %s", pkg, e)
        return None, None

# --- per-app chat session files (single source of truth) -------------------
# ~/.scrcpy_ai/db/chats/{pkg}.jsonl — append-only. 웹UI는 이 파일을 "보여주기만" 한다.
CHAT_DIR = os.path.expanduser("~/.scrcpy_ai/db/chats")
RESTORE_ROUNDS = 10   # 서버 재시작 후 _history에 주입할 최근 대화 왕복 수


def _chat_file(pkg: str) -> str:
    return os.path.join(CHAT_DIR, f"{pkg}.jsonl")


def _chat_append(pkg: str, role: str, text: str):
    """세션 파일에 이벤트 append. role: user/bot/tool/error/summary."""
    try:
        os.makedirs(CHAT_DIR, exist_ok=True)
        with open(_chat_file(pkg), "a", encoding="utf-8") as f:
            f.write(json.dumps({"ts": time.time(), "role": role, "text": text},
                               ensure_ascii=False) + "\n")
    except OSError as e:
        logger.warning("chat append failed: %s", e)


def _chat_read(pkg: str, limit: int = 500):
    """세션 파일 읽기 (최근 limit건). 없으면 빈 리스트."""
    path = _chat_file(pkg)
    if not os.path.isfile(path):
        return []
    try:
        with open(path, encoding="utf-8") as f:
            lines = [l for l in f.read().splitlines() if l.strip()]
        out = []
        for l in lines[-limit:]:
            try:
                out.append(json.loads(l))
            except json.JSONDecodeError:
                continue
        return out
    except OSError:
        return []


@router.get("/skills")
async def agent_skills():
    """포그라운드 패키지 감지 → 매칭 스킬 목록."""
    if game_agent is None:
        return {"package": "", "skills": [], "error": "game_agent unavailable"}
    loop = asyncio.get_event_loop()
    pkg = await loop.run_in_executor(None, game_agent.get_foreground_package)
    rows = await loop.run_in_executor(None, game_agent.list_skills, pkg or "")
    png, label = await loop.run_in_executor(None, _extract_icon, pkg or "")
    return {
        "package": pkg or "",
        "label": label or "",
        "icon": f"/agent/icon/{pkg}" if png else "",
        "skills": [
            {"filename": f, "package": p, "created": c} for f, p, c in rows
        ],
    }


@router.get("/icon/{pkg}")
async def agent_icon(pkg: str):
    """캐시된 앱 아이콘 PNG 서빙."""
    if not _PKG_RE.match(pkg):
        return Response(status_code=404)
    png = os.path.join(_ICON_DIR, f"{pkg}.png")
    if not os.path.isfile(png):
        png, _ = await asyncio.get_event_loop().run_in_executor(
            None, _extract_icon, pkg)
    if not png or not os.path.isfile(png):
        return Response(status_code=404)
    return FileResponse(png, media_type="image/png")


@router.get("/skill/{filename}")
async def agent_skill(filename: str):
    if game_agent is None:
        return {"error": "game_agent unavailable"}
    text = game_agent.read_skill(filename)
    return {"filename": filename, "content": text}


@router.get("/history")
async def agent_history():
    """현재 포그라운드 앱의 세션 파일(전체 대화) — 웹UI 뷰어용."""
    if game_agent is None:
        return {"package": "", "messages": []}
    loop = asyncio.get_event_loop()
    pkg = await loop.run_in_executor(None, game_agent.get_foreground_package)
    rows = await loop.run_in_executor(None, _chat_read, pkg or "")
    return {"package": pkg or "", "messages": rows}


@router.get("/status")
async def agent_status():
    """에이전트 실제 상태 — 턴 실행 중(진행 중) 여부는 세션 락으로 판단."""
    busy = bool(_session is not None and _session._lock.locked())
    return {"busy": busy, "auto": _auto["on"],
            "auto_turns": _auto["turns"], "auto_max": AUTO_MAX_TURNS,
            "goal": _auto["goal"]}


@router.post("/stop")
async def agent_stop():
    """진행 중인 턴 즉시 중단 — 현재 await 지점에서 CancelledError."""
    if _auto["on"]:
        _auto_stop("사용자 중단")
    if _turn_task is None or _turn_task.done():
        return {"stopped": False}
    _turn_task.cancel()
    return {"stopped": True}


@router.post("/auto")
async def agent_auto(request: Request, goal: str = ""):
    """자동 진행(턴 체인) 토글. on: 턴 종료 시 서버가 이어서 진행."""
    loop = asyncio.get_event_loop()
    if _auto["on"]:
        _auto_stop("사용자가 자동 진행을 끔")
        if _turn_task is not None and not _turn_task.done():
            _turn_task.cancel()
        return {"auto": False}

    if game_agent is None:
        return {"error": "game_agent unavailable"}
    if _turn_task is not None and not _turn_task.done():
        return {"error": "진행 중인 턴이 끝난 뒤 켜주세요"}
    # goal: 쿼리스트링 우선, 없으면 폼 body (JS는 폼으로 보냄)
    if not goal:
        try:
            form = await request.form()
            goal = (form.get("goal") or "").strip()
        except Exception:
            pass
    pkg = await loop.run_in_executor(None, game_agent.get_foreground_package)
    _auto.update({"on": True, "turns": 0,
                  "goal": goal.strip() or "직전 목표를 이어서 완주"})
    # 현재 턴이 없으면 체인을 즉시 시작 (WS 재연결 없이도 동작)
    _auto["chain"] = asyncio.get_event_loop().create_task(
        _run_auto_from_scratch(pkg))
    _chat_append(pkg, "tool",
                 f"🚀 자동 진행 시작 (상한 {AUTO_MAX_TURNS}턴) — 목표: {_auto['goal']}")
    return {"auto": True, "max_turns": AUTO_MAX_TURNS, "goal": _auto["goal"]}


async def _run_auto_from_scratch(pkg: str):
    """WS 없는 자동 시작: 세션을 만들어 체인 첫 턴부터 실행."""
    session = _get_session(pkg)
    await _auto_chain(session, pkg)


# --- 화면 녹화 → 자동 분석 → 게임북 업데이트 -------------------------------
# 녹화 = 일정 간격 스크린샷 프레임 저장. 종료하면 z.ai 비전으로 분석해
# 기존 게임북(md, save_skill와 동일 형식)에 "관찰된 진행 방법"을 추가.
REC_DIR = os.path.expanduser("~/.scrcpy_ai/db/recordings")
REC_INTERVAL = 3.0        # 프레임 캡처 간격(초)
REC_MAX_FRAMES = 600      # 30분 상한
_ANALYSIS_FRAMES = 8      # 분석에 사용할 프레임 수

_rec_state = {"dir": None, "task": None}


def _rec_capture_frame(rec_dir: str, idx: int):
    """C 백엔드에서 스크린샷 1장 저장."""
    jpeg = game_agent._scrcpy_get("/internal/screenshot")
    with open(os.path.join(rec_dir, f"f{idx:05d}.jpg"), "wb") as f:
        f.write(jpeg)


async def _rec_loop(pkg: str):
    rec_dir = _rec_state["dir"]
    idx = 0
    try:
        while idx < REC_MAX_FRAMES:
            await asyncio.get_event_loop().run_in_executor(
                None, _rec_capture_frame, rec_dir, idx)
            idx += 1
            await asyncio.sleep(REC_INTERVAL)
    except asyncio.CancelledError:
        raise
    except Exception as e:
        logger.warning("recording frame failed: %s", e)


def _rec_b64(path: str) -> str:
    import base64
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


def _rec_analyze(pkg: str, rec_dir: str) -> str:
    """녹화 프레임 샘플링 → z.ai 비전 분석 → 게임북 md 업데이트. 요약 반환."""
    frames = sorted(f for f in os.listdir(rec_dir) if f.endswith(".jpg"))
    if not frames:
        return "녹화된 프레임이 없어 분석을 건너뛰었습니다."
    # 균등 샘플링
    step = max(1, len(frames) // _ANALYSIS_FRAMES)
    sample = frames[::step][:_ANALYSIS_FRAMES]
    content = [{"type": "text", "text":
                "한 모바일 게임 녹화의 순차 프레임들이다. 이 녹화에서 수행된 "
                "진행 방법(목표 → 조작 흐름 → 결과)을 분석해, 게임 진행 "
                "가이드(md) 항목으로 정리해라:\n"
                "- 무엇을 목표로 했고 (예: 특정 스테이지 돌파)\n"
                "- 화면이 어떻게 흘렀고 (중요 순간별 화면 상태)\n"
                "- 어떤 조작이 필요한지 (버튼 위치 중심, 좌표는 참고용)\n"
                "- 주의점 (실패했던 흐름, 되돌리기 등)\n"
                "500자 이내 개조식. 좌표 추정이 불확실하면 좌표 대신 요소명으로."}]
    for fn in sample:
        content.append({"type": "image_url", "image_url": {
            "url": "data:image/jpeg;base64," + _rec_b64(os.path.join(rec_dir, fn))}})
    msg, _ = game_agent._chat([{"role": "user", "content": content}], None)
    analysis = (msg.get("content") or "").strip() or "분석 결과 없음"

    # 게임북 업데이트 — save_skill와 동일 위치/형식 (패키지별)
    skill_dir = game_agent.SKILL_DIR
    skill_dir.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    path = skill_dir / f"{pkg or 'unknown'}-녹화분석-{ts}.md"
    body = [
        "---",
        f"package: {pkg or 'unknown'}",
        f"created: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "actions_count: 0",
        "source: screen-recording",
        "---",
        "",
        "# 녹화 분석: 사용자 플레이에서 추출한 진행 방법",
        "",
        analysis,
        "",
        "## 활용 지침",
        "- 스크린샷을 먼저 찍어 현재 상태를 확인하고 위 흐름에 맞게 진행한다.",
        "- 좌표는 기록 시점 화면 기준 — 현재 화면에 맞게 보정한다.",
        "- 패키지가 다른 게임에서는 이 스킬을 적용하지 않는다.",
    ]
    path.write_text("\n".join(body), encoding="utf-8")
    return f"녹화 분석 완료 → 게임북 업데이트: {os.path.basename(path)}\n\n{analysis}"


@router.post("/record")
async def agent_record():
    """녹화 토글. 시작→프레임 캡처, 종료→자동 분석+게임북 업데이트."""
    loop = asyncio.get_event_loop()
    if _rec_state["task"] is not None:
        # --- 종료: 캡처 루프 취소 후 자동 분석 (백그라운드) ---
        _rec_state["task"].cancel()
        _rec_state["task"] = None
        rec_dir, _rec_state["dir"] = _rec_state["dir"], None
        pkg = game_agent.get_foreground_package() if game_agent else ""

        async def _finalize():
            try:
                _chat_append(pkg, "tool", "● 녹화 종료 — 자동 분석 중…")
                summary = await loop.run_in_executor(
                    None, _rec_analyze, pkg, rec_dir)
                _chat_append(pkg, "bot", summary)
            except Exception as e:
                logger.warning("record analysis failed: %s", e)
                _chat_append(pkg, "error", f"녹화 분석 실패: {e}")

        loop.create_task(_finalize())
        return {"recording": False}

    # --- 시작 ---
    if game_agent is None:
        return {"error": "game_agent unavailable"}
    pkg = await loop.run_in_executor(None, game_agent.get_foreground_package)
    ts = time.strftime("%Y%m%d-%H%M%S")
    rec_dir = os.path.join(REC_DIR, f"{pkg or 'unknown'}-{ts}")
    os.makedirs(rec_dir, exist_ok=True)
    _rec_state["dir"] = rec_dir
    _rec_state["task"] = loop.create_task(_rec_loop(pkg))
    _chat_append(pkg, "tool", "● 녹화 시작 (3초 간격 화면 캡처)")
    return {"recording": True, "dir": rec_dir}


# 단일 세션 (웹UI는 1명 사용) — GameAgentSession은 asyncio.Lock으로 턴 직렬화.
_session = None
_session_pkg = None
_turn_task = None   # 진행 중 턴 태스크 (STOP 취소용)

# --- 자동 진행(턴 체인): 턴이 끝나면 서버가 스스로 "이어서 진행" ----------
# 브라우저를 닫아도 서버에서 게임 사이클이 끝날 때까지 진행한다.
# 정지 조건: (1) 모델의 end_game_cycle 선언 (2) AUTO_MAX_TURNS 도달
#            (3) 사용자의 ■ 중단 / 자동 진행 OFF
AUTO_MAX_TURNS = int(os.environ.get("AGENT_AUTO_MAX_TURNS", "20"))
_auto = {"on": False, "chain": None, "turns": 0, "goal": ""}


def _auto_stop(reason: str, pkg: str = ""):
    if _auto["chain"] is not None:
        _auto["chain"].cancel()
        _auto["chain"] = None
    _auto["on"] = False
    _auto["turns"] = 0
    if pkg:
        _chat_append(pkg, "tool", f"⏹ 자동 진행 종료 — {reason}")
    logger.info("auto chain stopped: %s", reason)


async def _auto_chain(session, pkg: str):
    """WS 없이 서버 내부에서 턴을 이어붙이는 루프. 원 턴은 이미 완료된 상태."""
    loop = asyncio.get_event_loop()
    try:
        while _auto["on"] and _auto["turns"] < AUTO_MAX_TURNS:
            if session.cycle_done:
                _chat_append(pkg, "tool", "🏁 게임 사이클 완료 선언 — 자동 진행 정지")
                break
            await asyncio.sleep(2)   # 직전 턴 정리(파일 flush) 여유
            _auto["turns"] += 1
            _chat_append(pkg, "tool",
                         f"▶ 자동 진행 턴 {_auto['turns']}/{AUTO_MAX_TURNS}"
                         f" — 목표: {_auto['goal']}")
            def _on_tool(name, desc):
                _chat_append(pkg, "tool", f"⚙ {name} {desc or ''}".strip())
            try:
                reply = await session.send(
                    f"[자동 진행] 이어서 진행해라. 목표: {_auto['goal']}",
                    on_tool=_on_tool)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.warning("auto turn failed: %s", e)
                _chat_append(pkg, "error", f"자동 진행 턴 실패: {e}")
                await asyncio.sleep(5)
                continue
            if getattr(session, "_summary", ""):
                _chat_append(pkg, "summary", "[세션 요약] " + session._summary)
            if session.cycle_done:
                _chat_append(pkg, "tool", "🏁 게임 사이클 완료 선언 — 자동 진행 정지")
                break
        else:
            if _auto["on"]:
                _auto_stop(f"자동 턴 상한({AUTO_MAX_TURNS}) 도달", pkg)
    except asyncio.CancelledError:
        pass
    finally:
        _auto["on"] = False
        _auto["chain"] = None
        _auto["turns"] = 0


def _prime_history(session, pkg: str):
    """서버 재시작 등으로 세션이 비었으면 세션 파일에서 맥락 주입."""
    if session._history:
        return
    rows = _chat_read(pkg, limit=RESTORE_ROUNDS * 2 + 10)
    summary = next((r["text"] for r in reversed(rows)
                    if r.get("role") == "summary"), "")
    if summary:
        session._summary = summary.removeprefix("[세션 요약] ").strip()
    texts = [r for r in rows if r.get("role") in ("user", "bot")]
    for r in texts[-RESTORE_ROUNDS * 2:]:
        role = "user" if r["role"] == "user" else "assistant"
        session._history.append({"role": role, "content": r["text"]})
    if session._history:
        logger.info("session primed from %s (%d msgs, summary=%s)",
                    _chat_file(pkg), len(session._history),
                    bool(summary))


def _get_session(pkg: str):
    """앱 패키지별 세션. 세션이 없거나 앱이 바뀌면 (파일에서 복구해) 생성."""
    global _session, _session_pkg
    if game_agent is None:
        raise RuntimeError("game_agent 모듈을 로드할 수 없습니다 "
                           "(scrcpy-ai 로그 확인)")
    if _session is None or _session_pkg != pkg:
        if _session is not None:
            try:
                asyncio.get_event_loop().create_task(_session.close())
            except Exception:
                pass
        _session = game_agent.GameAgentSession()
        _session_pkg = pkg
        _prime_history(_session, pkg)
    return _session


@router.post("/reset")
async def agent_reset():
    """세션 초기화 — 현재 앱의 대화 파일을 아카이브하고 새 맥락 시작."""
    global _session, _session_pkg
    loop = asyncio.get_event_loop()
    pkg = await loop.run_in_executor(None, game_agent.get_foreground_package) \
        if game_agent else ""
    if _session is not None:
        try:
            await _session.close()
        except Exception as e:
            logger.warning("agent reset close failed: %s", e)
        _session = None
        _session_pkg = None
    if pkg:
        src = _chat_file(pkg)
        if os.path.isfile(src):
            dst = src + "." + time.strftime("%Y%m%d-%H%M%S") + ".bak"
            try:
                os.replace(src, dst)
                logger.info("chat archived: %s", dst)
            except OSError as e:
                logger.warning("chat archive failed: %s", e)
    return {"ok": True}


@router.websocket("/ws/agent/chat")
async def agent_chat(ws: WebSocket):
    """채팅 WS — main.py의 _ws_authorized를 이미 통과한 상태로만 연결된다."""
    await ws.accept()
    pkg = ""
    try:
        while True:
            msg = await ws.receive_json()
            if msg.get("type") != "user":
                continue
            text = (msg.get("text") or "").strip()
            if not text:
                continue
            pkg = game_agent.get_foreground_package() if game_agent else ""
            _chat_append(pkg, "user", text)

            def on_tool(name: str, desc: str):
                # game_agent는 콜백을 동기 호출한다 — send_json을 루프에 스케줄.
                try:
                    loop = asyncio.get_event_loop()
                    loop.create_task(ws.send_json(
                        {"type": "tool", "name": name, "desc": desc}))
                    _chat_append(pkg, "tool",
                                 f"⚙ {name} {desc or ''}".strip())
                except Exception:
                    pass

            session = _get_session(pkg)
            global _turn_task
            _turn_task = asyncio.current_task()
            try:
                reply = await session.send(text, on_tool=on_tool)
            finally:
                _turn_task = None
            _chat_append(pkg, "bot", reply)
            # 압축으로 요약이 갱신됐으면 세션 파일에도 남긴다 (재시작 복구용)
            if getattr(session, "_summary", ""):
                _chat_append(pkg, "summary", "[세션 요약] " + session._summary)
            await ws.send_json({"type": "reply", "text": reply})
            # 자동 진행 켜짐 → 이 턴 뒤에 서버가 체인을 잇는다
            if _auto["on"] and _auto["chain"] is None:
                _auto["chain"] = asyncio.get_event_loop().create_task(
                    _auto_chain(session, pkg))
    except asyncio.CancelledError:
        if pkg:
            _chat_append(pkg, "tool", "■ 사용자가 진행을 중단했습니다")
        try:
            await ws.send_json({"type": "tool",
                                "name": "stopped",
                                "desc": "사용자가 진행을 중단했습니다"})
        except Exception:
            pass
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.warning("agent chat error: %s", e)
        if pkg:
            _chat_append(pkg, "error", str(e))
        try:
            await ws.send_json({"type": "error", "text": str(e)})
        except Exception:
            pass
    finally:
        try:
            await ws.close()
        except Exception:
            pass
