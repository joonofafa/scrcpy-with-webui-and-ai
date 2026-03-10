"""Core AI agent: hybrid decision loop (Memory → VLM fallback)."""

import json
import logging
import threading
import time

from scrcpy_ai.config import config
from scrcpy_ai.db.action_history import ActionHistoryWindow
from scrcpy_ai.device import client as device
from scrcpy_ai.device.tool_executor import execute as execute_tool
from scrcpy_ai.device.tools import TOOL_DEFINITIONS
from scrcpy_ai.llm.openrouter import analyze_screen, chat_completion

logger = logging.getLogger(__name__)

SYSTEM_PROMPT = (
    "You are an AI assistant controlling an Android device via scrcpy.\n\n"
    "*** FUNCTION CALLING IS MANDATORY ***\n"
    "You have tools: position_click, position_long_press, swipe, "
    "key_press, input_text, screenshot.\n"
    "You MUST use function calling (tool_calls) to invoke them.\n"
    "NEVER output JSON in text. NEVER write code blocks with actions.\n"
    "ALWAYS use the tool_calls mechanism provided by the API.\n\n"
    "SCREEN COORDINATES:\n"
    "- Screenshot caption shows 'Screenshot WxH' = actual pixel dimensions.\n"
    "- Valid range: X: 0..W-1, Y: 0..H-1. (0,0) = top-left.\n"
    "- ONLY use cx/cy values from VLM analysis. NEVER guess.\n\n"
    "ACTION PROTOCOL:\n"
    "1. State which element you will tap and WHY (brief).\n"
    "2. Call the tool via function calling.\n"
    "3. Call screenshot() to verify the result.\n"
    "4. If screen unchanged -> tap MISSED -> try different element.\n\n"
    "AUTONOMOUS MODE:\n"
    "- Follow game rules step by step. NEVER ask the user.\n"
    "- Every response MUST include at least one tool call.\n"
    "- If unsure, call screenshot() first, then act on what you see."
)

MAX_MESSAGES = 40
MAX_ITERATIONS = 50


class AIAgent:
    def __init__(self):
        self.messages: list[dict] = [{"role": "system", "content": SYSTEM_PROMPT}]
        self.activity_log: list[dict] = []  # {role, content} for UI display
        self.lock = threading.Lock()

        # State
        self.auto_running = False
        self.recording = False
        self.record_count = 0
        self.game_rules = ""
        self.screen_width = 0
        self.screen_height = 0

        # CLIP (legacy mode)
        self.clip_embeddings: list[dict] | None = None

        # Train tree
        self.train_tree: dict | None = None

        # Hybrid mode
        self.hybrid_enabled = True
        self.action_history = ActionHistoryWindow()
        self._prev_phash: str | None = None
        self._same_screen_count = 0

        # Auto-play thread
        self._auto_thread: threading.Thread | None = None
        self._stop_event = threading.Event()

    def _log(self, role: str, content: str):
        """Add to activity log (shown in web UI)."""
        with self.lock:
            self.activity_log.append({"role": role, "content": content})
            if len(self.activity_log) > 200:
                self.activity_log = self.activity_log[-200:]

    def _trim_messages(self):
        """Keep message history bounded."""
        if len(self.messages) <= MAX_MESSAGES:
            return
        keep = MAX_MESSAGES - 1
        start = len(self.messages) - keep
        while start < len(self.messages) and self.messages[start].get("role") == "tool":
            start -= 1
        if start < 1:
            start = 1
        self.messages = [self.messages[0]] + self.messages[start:]

    def clear_history(self):
        with self.lock:
            self.messages = [{"role": "system", "content": SYSTEM_PROMPT}]
            self.activity_log = []
        self.action_history.clear()
        self._prev_phash = None
        self._same_screen_count = 0

    def _take_screenshot_with_vlm(self) -> str | None:
        """Capture screenshot, analyze with VLM, add to messages. Returns VLM description."""
        ss = device.screenshot()
        if not ss:
            return None

        self.screen_width = ss.screenshot_w
        self.screen_height = ss.screenshot_h

        description = None
        if config.vision_model:
            description = analyze_screen(ss.base64_data, ss.screenshot_w, ss.screenshot_h)

        if description:
            text = (
                f"Screenshot {ss.screenshot_w}x{ss.screenshot_h} (fresh)\n"
                f"=== VLM ANALYSIS (use these EXACT coordinates) ===\n"
                f"{description}\n"
                f"=== END VLM ANALYSIS ==="
            )
            self.messages.append({"role": "user", "content": text})
        else:
            text = f"Screenshot {ss.screenshot_w}x{ss.screenshot_h} (fresh)"
            self.messages.append({
                "role": "user",
                "content": [
                    {"type": "text", "text": text},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/jpeg;base64,{ss.base64_data}",
                    }},
                ],
            })

        return description

    def _save_vlm_action_to_memory(self, ss_jpeg: bytes, embedding, phash_hex,
                                   fn_name: str, args: dict):
        """Save a VLM-decided action to memory DB for future reuse."""
        from scrcpy_ai.db.memory_manager import memory

        if embedding is None:
            return

        action_type = fn_name
        x = args.get("x", args.get("x1", 0))
        y = args.get("y", args.get("y1", 0))
        x2 = args.get("x2")
        y2 = args.get("y2")
        extra = {k: v for k, v in args.items()
                 if k not in ("x", "y", "x1", "y1", "x2", "y2")}

        state_id = memory.save_experience(
            jpeg_bytes=ss_jpeg,
            embedding=embedding,
            action_type=action_type,
            x=x, y=y, x2=x2, y2=y2,
            extra_json=json.dumps(extra) if extra else None,
            phash_hex=phash_hex,
        )

        self.action_history.add(state_id, action_type, x, y)
        logger.info("Saved VLM action to memory: state=%s %s(%d,%d)",
                     state_id, action_type, x, y)

    def process_prompt(self, prompt: str, _capture_context: dict | None = None):
        """Process a user prompt through the LLM loop.

        _capture_context: optional dict with {jpeg_bytes, embedding, phash_hex}
            used in hybrid mode to save VLM actions to memory.
        """
        self.messages.append({"role": "user", "content": prompt})
        self._log("user", prompt)

        consecutive_text = 0
        for iteration in range(MAX_ITERATIONS):
            if self._stop_event.is_set():
                logger.info("Agent stopped during process_prompt")
                break

            logger.info("LLM call iteration %d (auto=%s)", iteration, self.auto_running)

            self._trim_messages()
            result = chat_completion(self.messages, TOOL_DEFINITIONS)

            if "error" in result:
                self._log("assistant", f"[Error] {result['error']}")
                break

            content = result.get("content")
            tool_calls = result.get("tool_calls")

            if tool_calls:
                consecutive_text = 0

                assistant_msg = {"role": "assistant", "tool_calls": tool_calls}
                if content:
                    assistant_msg["content"] = content
                    self._log("assistant", content)
                self.messages.append(assistant_msg)

                for tc in tool_calls:
                    fn_name = tc["function"]["name"]
                    try:
                        args = json.loads(tc["function"]["arguments"])
                    except json.JSONDecodeError:
                        args = {}

                    if fn_name == "screenshot":
                        self._take_screenshot_with_vlm()
                        tool_result = json.dumps({
                            "success": True,
                            "width": self.screen_width,
                            "height": self.screen_height,
                        })
                        self._log("assistant",
                                  f"[Screenshot] {self.screen_width}x{self.screen_height}")
                    else:
                        result_dict = execute_tool(fn_name, args)
                        tool_result = json.dumps(result_dict)
                        self._log("assistant",
                                  f"[Tool] {fn_name}({json.dumps(args)}) -> {tool_result[:200]}")

                        # Save VLM action to memory DB (hybrid mode)
                        if (_capture_context and fn_name in
                                ("position_click", "position_long_press", "swipe")):
                            self._save_vlm_action_to_memory(
                                _capture_context["jpeg_bytes"],
                                _capture_context["embedding"],
                                _capture_context["phash_hex"],
                                fn_name, args,
                            )

                    self.messages.append({
                        "role": "tool",
                        "tool_call_id": tc["id"],
                        "content": tool_result,
                    })

                continue

            if content:
                self._log("assistant", content)
                self.messages.append({"role": "assistant", "content": content})

            if self.clip_embeddings:
                break

            if self.auto_running and not self._stop_event.is_set():
                consecutive_text += 1
                if consecutive_text >= 3:
                    logger.warning("3 consecutive text-only responses, stopping loop")
                    break
                time.sleep(0.5)
                self._take_screenshot_with_vlm()
                continue

            break

    # ── Auto-play control ──────────────────────────────────────────────

    def start_auto(self):
        """Start auto-play loop."""
        if self.auto_running:
            return
        self.auto_running = True
        self._stop_event.clear()
        self.activity_log = []
        self._prev_phash = None
        self._same_screen_count = 0
        self._auto_thread = threading.Thread(target=self._auto_loop, daemon=True)
        self._auto_thread.start()

    def stop_auto(self):
        """Stop auto-play loop."""
        self.auto_running = False
        self._stop_event.set()

    def _auto_loop(self):
        """Main auto-play loop: Hybrid Decision Loop.

        Priority:
          1. CLIP legacy mode (if clip_embeddings loaded)
          2. Hybrid mode (pHash → Memory → VLM fallback)
          3. Rules-only mode (pure VLM, no memory)
        """
        logger.info("Auto-play started (hybrid=%s)", self.hybrid_enabled)
        rules_sent = False

        while not self._stop_event.is_set():
            # Legacy CLIP mode
            if self.clip_embeddings:
                self._clip_auto_cycle()
                if self._stop_event.wait(2.0):
                    break
                continue

            # Hybrid mode (needs game_rules for VLM fallback)
            if self.hybrid_enabled and self.game_rules:
                self._hybrid_cycle(rules_sent)
                rules_sent = True
                if self._stop_event.wait(1.0):
                    break
                continue

            # Rules-only mode (pure VLM)
            if self.game_rules:
                if not rules_sent:
                    prompt = (
                        "Follow the game rules below, look at the screenshot, "
                        "and use the available tools to play.\n\n"
                        + self.game_rules
                    )
                    rules_sent = True
                else:
                    prompt = "Take a screenshot and continue playing."
                self.process_prompt(prompt)
            else:
                if self._stop_event.wait(1.0):
                    break
                continue

            if self._stop_event.wait(1.0):
                break

        self.auto_running = False
        logger.info("Auto-play stopped")

    # ── Hybrid Decision Cycle ──────────────────────────────────────────

    def _hybrid_cycle(self, rules_sent: bool):
        """One cycle of hybrid decision: pHash → Memory → VLM fallback."""
        from scrcpy_ai.clip.matcher import (
            compute_phash,
            embed_image_bytes,
            get_best_action,
            phash_distance,
        )
        from scrcpy_ai.db.memory_manager import memory

        # 1. Capture screenshot
        ss = device.screenshot()
        if not ss:
            self._log("assistant", "[Hybrid] No frame available")
            return

        self.screen_width = ss.screenshot_w
        self.screen_height = ss.screenshot_h

        # 2. Change Detection (pHash)
        cur_phash = compute_phash(ss.jpeg_bytes)
        if cur_phash and self._prev_phash:
            dist = phash_distance(cur_phash, self._prev_phash)
            if dist < config.phash_threshold:
                self._same_screen_count += 1
                if self._same_screen_count >= config.max_same_screen:
                    logger.info("Hybrid: same screen %dx, forcing VLM",
                                self._same_screen_count)
                    # Fall through to VLM
                else:
                    logger.debug("Hybrid: screen unchanged (dist=%d), skipping", dist)
                    return
            else:
                self._same_screen_count = 0
        self._prev_phash = cur_phash

        # 3. CLIP embedding
        cur_emb = embed_image_bytes(ss.jpeg_bytes)
        if cur_emb is None:
            self._log("assistant", "[Hybrid] Embedding failed, falling back to VLM")
            self._vlm_fallback(ss, rules_sent, None, cur_phash)
            return

        from scrcpy_ai.db.memory_manager import MemoryManager
        cur_state_id = MemoryManager._make_state_id(cur_emb)

        # 4. Memory Match
        best = get_best_action(cur_emb, cur_state_id, self.action_history)

        if best and best["score"] > 0:
            # Memory hit — execute action without VLM
            action_type = best["action_type"]
            x, y = best["x"], best["y"]

            self._log("assistant",
                      f"[Memory] {action_type}({x},{y}) "
                      f"sim={best['similarity']:.3f} score={best['score']:.3f} "
                      f"exec={best['execution_count']}")

            if action_type == "swipe" and best.get("x2") is not None:
                device.swipe(x, y, best["x2"], best["y2"])
            elif action_type == "position_long_press":
                extra = json.loads(best.get("extra_json") or "{}")
                device.long_press(x, y, extra.get("duration_ms", 500))
            else:
                device.click(x, y)

            # Record in history
            self.action_history.add(cur_state_id, action_type, x, y)

            # Update execution count in DB
            memory.save_experience(
                jpeg_bytes=ss.jpeg_bytes,
                embedding=cur_emb,
                action_type=action_type,
                x=x, y=y,
                x2=best.get("x2"), y2=best.get("y2"),
                phash_hex=cur_phash,
            )

            self._same_screen_count = 0
            return

        # 5. All candidates tried or no match → VLM Fallback
        if best:
            self._log("assistant",
                      f"[Hybrid] Memory actions exhausted (score={best['score']:.3f}), "
                      f"falling back to VLM")
        else:
            self._log("assistant", "[Hybrid] No memory match, falling back to VLM")

        self._vlm_fallback(ss, rules_sent, cur_emb, cur_phash)

    def _vlm_fallback(self, ss, rules_sent: bool,
                      embedding, phash_hex: str | None):
        """Fall back to VLM (Rules mode) and save the result to memory."""
        # Add screenshot to VLM context
        description = None
        if config.vision_model:
            description = analyze_screen(ss.base64_data, ss.screenshot_w, ss.screenshot_h)

        if description:
            text = (
                f"Screenshot {ss.screenshot_w}x{ss.screenshot_h} (fresh)\n"
                f"=== VLM ANALYSIS (use these EXACT coordinates) ===\n"
                f"{description}\n"
                f"=== END VLM ANALYSIS ==="
            )
            self.messages.append({"role": "user", "content": text})
        else:
            text = f"Screenshot {ss.screenshot_w}x{ss.screenshot_h} (fresh)"
            self.messages.append({
                "role": "user",
                "content": [
                    {"type": "text", "text": text},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/jpeg;base64,{ss.base64_data}",
                    }},
                ],
            })

        # Build capture context for saving VLM actions to memory
        capture_ctx = {
            "jpeg_bytes": ss.jpeg_bytes,
            "embedding": embedding,
            "phash_hex": phash_hex,
        }

        if not rules_sent:
            prompt = (
                "Follow the game rules below, look at the screenshot, "
                "and use the available tools to play.\n\n"
                + self.game_rules
            )
        else:
            prompt = "Continue playing based on the screenshot above."

        self.process_prompt(prompt, _capture_context=capture_ctx)
        self._same_screen_count = 0

    # ── Legacy modes ───────────────────────────────────────────────────

    def _clip_auto_cycle(self):
        """One cycle of CLIP-based auto-play (legacy)."""
        from scrcpy_ai.clip.matcher import clip_auto_cycle
        clip_auto_cycle(self)

    def _tree_auto_cycle(self):
        """One cycle of tree-based auto-play."""
        self._log("assistant", "[Tree] Tree-based play not yet implemented in Python")
        time.sleep(5)

    # ── State ──────────────────────────────────────────────────────────

    def get_play_mode(self) -> str:
        if self.clip_embeddings:
            return "clip"
        if self.hybrid_enabled and self.game_rules:
            return "hybrid"
        if self.train_tree and self.train_tree.get("states"):
            return "tree"
        if self.game_rules:
            return "rules"
        return "none"

    def get_state(self) -> dict:
        """Build state dict for web UI polling."""
        from scrcpy_ai.db.memory_manager import memory as mem_mgr

        with self.lock:
            messages_for_ui = []
            for m in self.messages:
                role = m.get("role", "")
                content = m.get("content", "")
                if isinstance(content, list):
                    parts = []
                    for part in content:
                        if isinstance(part, dict) and part.get("type") == "image_url":
                            parts.append("[Screenshot]")
                        elif isinstance(part, dict) and part.get("type") == "text":
                            parts.append(part.get("text", ""))
                    content = "\n".join(parts)
                msg = {"role": role, "content": content or ""}
                tc = m.get("tool_calls")
                if tc:
                    msg["tool_calls"] = json.dumps(tc)
                messages_for_ui.append(msg)

            # Memory stats (lazy, don't init DB just for this)
            try:
                mem_stats = mem_mgr.stats() if mem_mgr._initialized else {}
            except Exception:
                mem_stats = {}

            return {
                "screen_width": self.screen_width,
                "screen_height": self.screen_height,
                "messages": messages_for_ui,
                "auto_running": self.auto_running,
                "recording": self.recording,
                "record_count": self.record_count,
                "game_rules": self.game_rules,
                "play_mode": self.get_play_mode(),
                "hybrid_enabled": self.hybrid_enabled,
                "clip_count": len(self.clip_embeddings) if self.clip_embeddings else 0,
                "memory_stats": mem_stats,
                "history_size": len(self.action_history),
                "config_api_key": "sk-****" if len(config.api_key) > 8 else config.api_key,
                "config_model": config.model,
                "config_vision_model": config.vision_model,
                "config_base_url": config.base_url,
            }


# Global agent instance
agent = AIAgent()
