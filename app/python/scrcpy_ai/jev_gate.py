"""로컬 Jev-Style 0.8B 판정 — 루프(막힘) 탈출 판정 G3 전용.

모델: chaoliangUNSW/Jev-Style-0.8B-Decision-v3 (Q4_K_M, Vulkan iGPU 전층 오프로드)
(유료 재화 소비 하드게이트는 2026-11-02 제거 — Bill이 요청한 기능이 아니었음.)
"""

import logging
import threading

logger = logging.getLogger(__name__)

_DTA = "/home/jhsoft/gitHub/decision-test-agent"
_lock = threading.Lock()
_engine = None
_degraded = False


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
        _reset_engine()
        return None

