"""로컬 Jev-Style 0.8B 하드게이트 — 유료 재화 소비 조직 차단.

설계/검증 근거: ~/gitHub/scrcpy-with-webui-and-ai/HANDOFF_JEV_GATE.md
모델: chaoliangUNSW/Jev-Style-0.8B-Decision-v3 (Q4_K_M, Vulkan iGPU 전층 오프로드)

2층 방어 (실측 기반):
  1층 키워드(결정적): 가챠/뽑기/보석/gem/결제/구매/checkout/상점 등 등장 → 즉시 차단.
     근거: 모델이 ko-gacha를 0.31~0.55로 애매하게 판정(모델 한계 실측).
  2층 모델(noul): 키워드 미검출 시 로컬 0.8B 판정. note 필드로 무료 행동
     (광고/스태미너/우편) FP를 잡는 것까지 검증됨.
     영문 의도 0.70+ 신뢰, 한국어 소비 표현 일부 과소평가 → 1층이 커버.

fail-safe: 모델 실패 시 1층 키워드만으로 동작(통과 로그 남김).
"""

import json
import logging
import re
import threading

logger = logging.getLogger(__name__)

_DTA = "/home/jhsoft/gitHub/decision-test-agent"
GATE_THRESHOLD = 0.5
GATED_TOOLS = ("click", "long_press", "swipe")

# 1층: 결정적 키워드 (한/영, 대소문자 무관)
SPEND_KW = re.compile(
    r"가챠|가챠뽑기|뽑기|보석|젬|결제|구매|상점|인앱|구독|이벤트패스|"
    r"in.?app|gacha|gem[s]?\b|jewel|purchase|checkout|buy\b|shop|paid|premium|"
    r"real.?money|cash|subscription|battle.?pass", re.I)

_lock = threading.Lock()
_engine = None
_degraded = False

# G2: 사용자 지시가 소비를 명시적으로 승인했는가 (1회성)
# 승인 기록: {"지시 해시": 만료시각} — 같은 지시 연속 턴에서 재질문 방지
_auth_grants: dict[str, float] = {}
_AUTH_TTL = 600.0  # 승인 유효 10분


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
        return None


def gate_tool(intent_text: str, name: str, args: dict,
              user_instruction: str = "") -> tuple[bool, str]:
    """(허가여부, tool 응답에 붙일 안내문). 허가=False면 실행 금지.

    G2: user_instruction 이 소비를 명시적으로 승인하면(토큰 증거) 1회 허가.
    승인 판정은 순수 코드(정규식+동사 근접) — 모델이 사용자 지시를 왜곡 판정하는
    것을 원천 차단한다.
    """
    if name not in GATED_TOOLS:
        return True, ""
    intent = intent_text or ""
    if SPEND_KW.search(intent):
        if _authorized_by_user(intent, user_instruction):
            grant = _grant_auth(intent)
            return True, (f"[gate:authorized {grant}분] 사용자 명시 지시로 "
                          "유료 소비 1회 허가. 신중히 실행하고 결과를 보고해라.")
        return False, (f"[게이트 차단:키워드] 유료 소비 표현 감지. "
                       "사용자의 명시적 지시 없이는 실행할 수 없다. 무료 대안을 "
                       "시도하거나 턴을 마치고 사용자에게 보고해라.")
    # 2층: 로컬 모델 noul
    p = spend_probability(intent, name, args)
    if p is None:
        return True, "[gate:degraded]"
    if p < GATE_THRESHOLD:
        return True, f"[gate:ok P={p:.2f}]"
    return False, (f"[게이트 차단] 유료 재화 소비 가능성 P={p:.2f}. "
                   "사용자의 명시적 지시 없이는 실행할 수 없다. 무료 대안을 "
                   "시도하거나 턴을 마치고 사용자에게 보고해라.")


# ── G2: 사용자 지시 승인 판정 ──────────────────────────────────────
# 조건: 소비 키워드가 사용자 지시에 있고 + 실행 동사(해줘/실행/돌려/사줘/구매 등)가
# 같은 문장에 있어야 한다. "가챠 이벤트 뭐야?" 같은 질문형은 승인 아님.
_AUTH_VERBS = re.compile(
    r"해줘|해주라|해봐|실행|진행|돌려|돌리|사줘|사다|구매|구입|뽑아|뽑기|"
    r"승인|허락|허가|ok|okay|yes|do it|go ahead|buy|run|spend", re.I)
_NEGATION = re.compile(r"하지마|하지말|금지|no|don'?t|never|중지", re.I)


def _authorized_by_user(intent_text: str, user_instruction: str) -> bool:
    """사용자 지시 문장 자체가 소비 승인인가. (질문/부정은 제외)

    승인 근거 2종:
    a) 지시에 소비 키워드 + 실행 동사 ("가챠 돌려줘")
    b) 지시에 구매성 동사만 ("강화 아이템 사줘") — intent가 소비로 판정된
       상황에서만 성립하므로 안전
    """
    ui = (user_instruction or "").strip()
    if not ui or _NEGATION.search(ui):
        return False
    has_spend_kw = bool(SPEND_KW.search(ui))
    has_verb = bool(_AUTH_VERBS.search(ui))
    if has_spend_kw and has_verb:
        return True
    if not has_spend_kw and has_verb:
        # 구매성 동사 단독: 지시가 순수 질문인지 확인 (?로 끝나면 승인 아님)
        if _QUESTION_END.search(ui):
            return False
        # 동사가 구매/획득 계열인 경우만 (진행/실행 같은 일반 동사는 제외)
        if _PURCHASE_VERBS.search(ui):
            return True
    return False


_QUESTION_END = re.compile(r"\?\s*$|어때\?|뭐야\?|돼\?|됨\?|ㅋㅋ", re.I)
_PURCHASE_VERBS = re.compile(
    r"사줘|사다|사주세요|구매|구입|뽑아|뽑기|돌려|돌리|승인|허락|"
    r"buy|purchase|spend", re.I)


def _grant_auth(intent_text: str) -> int:
    """동일 intent 에 대해 1회 승인 부여. 남은 분 반환."""
    import time as _t
    key = (intent_text or "")[:200]
    now = _t.time()
    # 만료된 승인 정리
    for k in [k for k, exp in _auth_grants.items() if exp < now]:
        _auth_grants.pop(k, None)
    exp = now + _AUTH_TTL
    _auth_grants[key] = exp
    return int(_AUTH_TTL / 60)


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

