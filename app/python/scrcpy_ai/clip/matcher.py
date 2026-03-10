"""CLIP-based screen matching, pHash change detection, and hybrid auto-play."""

import io
import logging
import time
from typing import Optional

import numpy as np

from scrcpy_ai.config import config
from scrcpy_ai.device import client as device

logger = logging.getLogger(__name__)

# Lazy-loaded pHash
_phash_available = None

# Lazy-loaded CLIP model
_clip_model = None
_clip_preprocess = None
_clip_tokenizer = None


def _load_clip():
    """Load CLIP model (lazy, first call only)."""
    global _clip_model, _clip_preprocess, _clip_tokenizer
    if _clip_model is not None:
        return

    import open_clip
    import torch

    logger.info("Loading CLIP model: %s (%s)", config.clip_model, config.clip_pretrained)
    _clip_model, _, _clip_preprocess = open_clip.create_model_and_transforms(
        config.clip_model, pretrained=config.clip_pretrained,
    )
    _clip_model.eval()
    _clip_tokenizer = open_clip.get_tokenizer(config.clip_model)
    logger.info("CLIP model loaded")


def embed_image_bytes(jpeg_bytes: bytes) -> np.ndarray | None:
    """Get CLIP embedding for a JPEG image. Returns L2-normalized float32 array."""
    _load_clip()
    import torch
    from PIL import Image

    try:
        img = Image.open(io.BytesIO(jpeg_bytes)).convert("RGB")
        tensor = _clip_preprocess(img).unsqueeze(0)
        with torch.no_grad():
            emb = _clip_model.encode_image(tensor)
            emb = emb / emb.norm(dim=-1, keepdim=True)
        return emb.squeeze().cpu().numpy().astype(np.float32)
    except Exception as e:
        logger.error("CLIP embed failed: %s", e)
        return None


def cosine_similarity(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-8))


def compute_phash(jpeg_bytes: bytes) -> Optional[str]:
    """Compute perceptual hash of an image. Returns hex string or None."""
    global _phash_available
    if _phash_available is False:
        return None
    try:
        import imagehash
        from PIL import Image
        _phash_available = True
        img = Image.open(io.BytesIO(jpeg_bytes))
        h = imagehash.phash(img)
        return str(h)
    except ImportError:
        _phash_available = False
        logger.warning("imagehash not available, pHash disabled")
        return None
    except Exception as e:
        logger.error("pHash failed: %s", e)
        return None


def phash_distance(hash1: str, hash2: str) -> int:
    """Hamming distance between two hex pHash strings."""
    try:
        import imagehash
        h1 = imagehash.hex_to_hash(hash1)
        h2 = imagehash.hex_to_hash(hash2)
        return h1 - h2
    except Exception:
        return 999


def get_best_action(
    current_embedding: np.ndarray,
    current_state_id: str,
    action_history,
) -> Optional[dict]:
    """Query memory DB and select best action with history-aware scoring.

    Returns dict with {action_type, x, y, x2, y2, state_id, similarity, score}
    or None if no suitable action found.
    """
    from scrcpy_ai.db.memory_manager import memory

    experiences = memory.query_experience(current_embedding, top_k=5)
    if not experiences:
        return None

    # Filter by similarity threshold
    candidates = []
    for exp in experiences:
        if exp["similarity"] < config.memory_sim_threshold:
            continue
        for action in exp["actions"]:
            # Score = similarity - history penalty
            penalty = action_history.penalty(
                current_state_id,
                action["action_type"],
                action["x"], action["y"],
            )
            # Boost actions with higher success rate
            exec_count = action["execution_count"]
            succ_rate = action["success_count"] / max(exec_count, 1)
            score = exp["similarity"] + (succ_rate * 0.1) - penalty

            candidates.append({
                "action_type": action["action_type"],
                "x": action["x"],
                "y": action["y"],
                "x2": action.get("x2"),
                "y2": action.get("y2"),
                "extra_json": action.get("extra_json"),
                "state_id": exp["state_id"],
                "similarity": exp["similarity"],
                "score": score,
                "execution_count": exec_count,
            })

    if not candidates:
        return None

    # Sort by score descending
    candidates.sort(key=lambda c: c["score"], reverse=True)

    # Return best candidate (caller decides whether to use or fallback to VLM)
    best = candidates[0]
    logger.info(
        "Best action: %s (%d,%d) score=%.3f sim=%.3f exec=%d",
        best["action_type"], best["x"], best["y"],
        best["score"], best["similarity"], best["execution_count"],
    )
    return best


def clip_auto_cycle(agent):
    """One cycle of CLIP-based auto-play. Called from agent._clip_auto_cycle."""
    embeddings = agent.clip_embeddings
    if not embeddings:
        return

    # 1. Capture current screen
    ss = device.screenshot()
    if not ss:
        agent._log("assistant", "[CLIP] No frame available")
        return

    cur_emb = embed_image_bytes(ss.jpeg_bytes)
    if cur_emb is None:
        agent._log("assistant", "[CLIP] Embedding failed")
        return

    # 2. Find all candidates above threshold
    candidates = []
    for entry in embeddings:
        stored_emb = np.array(entry["embedding"], dtype=np.float32)
        sim = cosine_similarity(cur_emb, stored_emb)
        if sim >= config.clip_sim_threshold:
            x = entry.get("x", 0)
            y = entry.get("y", 0)
            idx = entry.get("index", -1)

            # Dedup: skip if same coords within 20px
            dup = False
            for c in candidates:
                if abs(c["x"] - x) < 20 and abs(c["y"] - y) < 20:
                    if sim > c["sim"]:
                        c["sim"] = sim
                        c["idx"] = idx
                    dup = True
                    break
            if not dup:
                candidates.append({"sim": sim, "x": x, "y": y, "idx": idx})

    if not candidates:
        agent._log("assistant", "[CLIP] No match above threshold")
        return

    # Sort by descending similarity
    candidates.sort(key=lambda c: c["sim"], reverse=True)
    logger.info("CLIP: %d candidates, top sim=%.3f", len(candidates), candidates[0]["sim"])

    # 3. Try each candidate, check for screen change
    for i, m in enumerate(candidates):
        if agent._stop_event.is_set():
            return

        device.click(m["x"], m["y"])
        agent._log("assistant",
                    f"[CLIP] #{m['idx']} (sim={m['sim']:.2f}) -> tap ({m['x']},{m['y']}) [{i+1}/{len(candidates)}]")

        # Last candidate: don't check screen change
        if i == len(candidates) - 1:
            break

        time.sleep(1.5)

        # Check screen change
        new_ss = device.screenshot()
        if new_ss:
            new_emb = embed_image_bytes(new_ss.jpeg_bytes)
            if new_emb is not None:
                screen_sim = cosine_similarity(cur_emb, new_emb)
                logger.info("CLIP: screen change check: sim=%.3f", screen_sim)
                if screen_sim < config.clip_screen_change_threshold:
                    logger.info("CLIP: screen changed after action %d", i + 1)
                    return

        logger.info("CLIP: screen unchanged, trying next...")

    # All tried
    agent._log("assistant", f"[CLIP] Tried {len(candidates)} actions - retrying...")
