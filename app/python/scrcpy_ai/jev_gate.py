"""로컬 Jev-Style 0.8B 하드게이트 — 유료 재화 소비 조직 차단.

설계/검증 근거: ~/gitHub/scrcpy-with-webui-and-ai/HANDOFF_JEV_GATE.md
모델: chaoliangUNSW/Jev-Style-0.8B-Decision-v3 (Q4_K_M, Vulkan iGPU 전층 오프로드)

2층 방어 (실측 기반):
  1층 키워드(결정적): 가챠/뽑기/보석/gem/결제/구매/checkout/상점 등 등장 → 즉시 차단.
     근거: 모델이 ko-gacha를 0.31~0.55로 애매하게 판정(모델 한계 실측).
  2층 모델(noul): 키워드 미검출 시 로컬 0.8B 판정. note 필드로 무료 행동
     (광고/스태미너/우편) FP를 잡는 것까지 검증됨.
     영문 의도 0.70+ 신뢰, 한국어 소비 표현 일부 과소평가 → 1층이 커버.

fail-closed: 모델 판정이 불가능하면 게이트 대상 조작을 차단한다(결제 게이트는
통과보다 정지가 안전). 실패한 엔진은 버리고 다음 호출에서 다시 띄운다.
"""

import json
import logging
import re
import threading

logger = logging.getLogger(__name__)

_DTA = "/home/jhsoft/gitHub/decision-test-agent"
GATE_THRESHOLD = 0.5
GATED_TOOLS = ("click", "long_press", "swipe", "input_text")
# key_press values that can confirm a focused button: enter(66), space(62),
# dpad_center(23), numpad_enter(160).
CONFIRM_KEYS = {"enter", "space", "66", "62", "23", "160"}
# Prefix of every block note — callers use it to tell blocked from executed.
BLOCK_PREFIX = "[게이트 차단"

# 1층: 결정적 키워드 (한/영, 대소문자 무관)
SPEND_KW = re.compile(
    r"가챠|가챠뽑기|뽑기|보석|젬|결제|구매|상점|인앱|구독|이벤트패스|"
    r"in.?app|gacha|gem[s]?\b|jewel|purchase|checkout|buy\b|shop|paid|premium|"
    r"real.?money|cash|subscription|battle.?pass", re.I)

_lock = threading.Lock()
_engine = None
_degraded = False

# G2 승인 기록: {사용자 턴 키: (만료시각, 남은 횟수)}
_auth_grants: dict = {}
_AUTH_TTL = 600.0  # 승인 유효 10분
_AUTH_BUDGET = 2   # 승인 1건당 유료 조작 수 (구매 + 확인창)


def _get_engine():
    global _engine
    with _lock:
        if _engine is None:
            import sys
            sys.path.insert(0, _DTA + "/jev-score")
            from jev_style_decision_gguf import JevStyleDecisionGGUF
            _engine = JevStyleDecisionGGUF(
                _DTA + "/jev-score",
                gguf=_DTA + "/models/jev-style-0.8b-q4km.gguf",
                n_gpu_layers=999)
        return _engine


def _reset_engine():
    """Drop a dead/broken engine so the next call respawns it — otherwise one
    crashed jev-score process would disable the model layer until restart."""
    global _engine
    with _lock:
        if _engine is not None:
            try:
                _engine.close()
            except Exception:  # noqa: BLE001
                pass
            _engine = None


def spend_probability(intent_text: str, tool_name: str, tool_args: dict):
    """P(유료 재화 소비). 실패 시 None (fail-safe)."""
    global _degraded
    try:
        state = json.dumps({
            "planned_action": tool_name,
            "args": tool_args,
            "agent_intent": (intent_text or "")[:300],
            "note": ("judge only premium/paid currency spend. "
                     "Ads, stamina, mail claims are free"),
        }, ensure_ascii=False)
        r = _get_engine().decide(
            state,
            "Would this action consume premium currency or make a purchase?",
            qtype="noul")
        _degraded = False
        return float(r["probabilities"]["true"])
    except Exception as e:  # noqa: BLE001 - 게이트는 죽으면 안 된다
        if not _degraded:
            logger.warning("[gate:degraded] %s", e)
            _degraded = True
        _reset_engine()
        return None


def _is_gated(name: str, args: dict) -> bool:
    if name in GATED_TOOLS:
        return True
    # Enter/space can confirm a focused purchase button.
    return (name == "key_press"
            and str(args.get("key", "")).strip().lower() in CONFIRM_KEYS)


def gate_tool(intent_text: str, name: str, args: dict,
              user_instruction: str = "", turn_id=None) -> tuple[bool, str]:
    """(허가여부, tool 응답에 붙일 안내문). 허가=False면 실행 금지.

    유료 소비로 판정된 조작(키워드 또는 모델)은 사용자의 명시적 승인이 있을 때만,
    그 사용자 턴(turn_id)에서 _AUTH_BUDGET회까지 허가한다. 승인 판정은 순수
    코드다 — 모델이 사용자 지시를 왜곡 판정하는 것을 원천 차단한다.
    """
    if not _is_gated(name, args):
        return True, ""
    intent = intent_text or ""
    if SPEND_KW.search(intent):
        reason = "[게이트 차단:키워드] 유료 소비 표현 감지."
    else:
        # 2층: 로컬 모델 noul
        p = spend_probability(intent, name, args)
        if p is None:
            return False, ("[게이트 차단:판정불가] 결제 판정 모델을 사용할 수 없어 "
                           "조작을 막았다. 턴을 마치고 사용자에게 보고해라.")
        if p < GATE_THRESHOLD:
            return True, f"[gate:ok P={p:.2f}]"
        reason = f"[게이트 차단] 유료 재화 소비 가능성 P={p:.2f}."
    if _authorized_by_user(user_instruction):
        left = _consume_grant(turn_id if turn_id is not None else user_instruction)
        if left is not None:
            return True, (f"[gate:authorized 남은 {left}회] 사용자 명시 승인으로 "
                          "허가. 신중히 실행하고 결과를 보고해라.")
        return False, ("[게이트 차단:승인 소진] 이번 승인으로 허용된 유료 조작을 "
                       "모두 썼다. 턴을 마치고 사용자에게 보고해라.")
    return False, (reason + " 사용자의 명시적 지시 없이는 실행할 수 없다. "
                   "무료 대안을 시도하거나 턴을 마치고 사용자에게 보고해라.")


# ── G2: 사용자 지시 승인 판정 ──────────────────────────────────────
# 명시적 동의 표현만 승인이다. "해줘/진행/돌려" 같은 일반 동사, 질문, 부정어,
# 자동 모드가 합성한 지시는 승인이 아니다 ("가챠 돌려줘" → 차단·보고 → "승인").
_AUTO_PREFIX = "[자동 진행]"
_CONSENT = re.compile(
    r"승인|허락|허가|결제\s*해|구매\s*해|구입\s*해|사\s*줘|사\s*주|질러|"
    r"써도\s*(?:돼|됨|좋)|사용해도\s*(?:돼|됨|좋)|"
    r"\b(?:approved?|buy|purchase|spend)\b", re.I)
_NEGATION = re.compile(
    r"하지\s*마|하지\s*말|쓰지\s*마|쓰지\s*말|사지\s*마|사지\s*말|말고|아껴|아끼|"
    r"나중에|금지|중지|안\s*돼|안\s*되|안\s*써|안\s*사|하지\s*않|"
    r"\b(?:no|not|don'?t|never|without|stop)\b", re.I)
_QUESTION_END = re.compile(r"\?\s*$|어때\?|뭐야\?|돼\?|됨\?|ㅋㅋ", re.I)


def _authorized_by_user(user_instruction: str) -> bool:
    """사용자 지시 문장 자체가 소비 승인인가."""
    ui = (user_instruction or "").strip()
    if not ui or ui.startswith(_AUTO_PREFIX):
        return False
    if _NEGATION.search(ui) or _QUESTION_END.search(ui):
        return False
    return bool(_CONSENT.search(ui))


def _consume_grant(key) -> int | None:
    """승인 1건(사용자 턴 1개)당 _AUTH_BUDGET회. 차감 후 남은 횟수, 소진 시 None."""
    import time as _t
    now = _t.time()
    for k in [k for k, (exp, _) in _auth_grants.items() if exp < now]:
        _auth_grants.pop(k, None)
    exp, left = _auth_grants.get(key, (now + _AUTH_TTL, _AUTH_BUDGET))
    if left <= 0:
        return None
    _auth_grants[key] = (exp, left - 1)
    return left - 1


def is_degraded() -> bool:
    return _degraded


# ── G3: 루프 탈출 판정 (기존 40px 좌표 휴리스틱의 시맨틱 업그레이드) ──
# 호출 시점: 기존 루프 가드(_tool_repeat >= 2) 트리거 시. VLM이 직전 조작 후
# 화면 묘사를 남겼다면 그 텍스트로, 없다면 '동일한 화면이 반복된다' 기본값.

def stuck_probability(screen_change_desc: str) -> float | None:
    """P(화면이 완전히 멈춤). 검증: 막힘 0.95 / 로딩·결과·팝업 0.04~0.07."""
    global _degraded
    try:
        r = _get_engine().decide(
            screen_change_desc or "The screen is completely identical to before.",
            "Is the screen completely identical to before, with no change at all?",
            qtype="noul")
        _degraded = False
        return float(r["probabilities"]["true"])
    except Exception as e:  # noqa: BLE001
        if not _degraded:
            logger.warning("[gate:g3 degraded] %s", e)
            _degraded = True
        return None

