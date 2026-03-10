"""FastAPI routes — all /api/* endpoints."""

import json
import logging
import os

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, Response

from scrcpy_ai.agent.agent import agent
from scrcpy_ai.clip.matcher import embed_image_bytes
from scrcpy_ai.config import config
from scrcpy_ai.pipeline import recorder

logger = logging.getLogger(__name__)
router = APIRouter()


# ── State ───────────────────────────────────────────────────────────
@router.get("/api/state")
async def get_state():
    return agent.get_state()


# ── Prompt ──────────────────────────────────────────────────────────
@router.post("/api/prompt")
async def post_prompt(request: Request):
    body = await request.json()
    prompt = body.get("prompt", "")
    if not prompt:
        raise HTTPException(400, "missing prompt")
    import threading
    threading.Thread(target=agent.process_prompt, args=(prompt,), daemon=True).start()
    return {"ok": True}


# ── Config ──────────────────────────────────────────────────────────
@router.post("/api/config")
async def post_config(request: Request):
    body = await request.json()
    if body.get("api_key"):
        config.api_key = body["api_key"]
    if body.get("model"):
        config.model = body["model"]
    if body.get("base_url"):
        config.base_url = body["base_url"]
    if body.get("vision_model"):
        config.vision_model = body["vision_model"]
    return {"ok": True}


# ── Game Rules ──────────────────────────────────────────────────────
@router.get("/api/game-rules")
async def get_game_rules():
    return {"rules": agent.game_rules}


@router.post("/api/game-rules")
async def post_game_rules(request: Request):
    body = await request.json()
    agent.game_rules = body.get("rules", "")
    return {"ok": True}


# ── Auto Play ───────────────────────────────────────────────────────
@router.post("/api/auto/start")
async def auto_start():
    agent.start_auto()
    return {"ok": True}


@router.post("/api/auto/stop")
async def auto_stop():
    agent.stop_auto()
    return {"ok": True}


# ── History ─────────────────────────────────────────────────────────
@router.post("/api/clear")
async def clear_history():
    agent.clear_history()
    return {"ok": True}


# ── Recording ───────────────────────────────────────────────────────
@router.post("/api/record/start")
async def record_start():
    agent.recording = True
    return {"ok": True}


@router.post("/api/record/stop")
async def record_stop():
    agent.recording = False
    return {"ok": True}


@router.post("/api/record/clear")
async def record_clear():
    agent.recording = False
    agent.record_count = 0
    return {"ok": True}


# ── Train Sessions ──────────────────────────────────────────────────
@router.get("/api/train/sessions")
async def train_sessions():
    return recorder.list_sessions()


@router.get("/api/train/session")
async def train_session(name: str):
    return recorder.get_session(name)


@router.post("/api/train/session/delete")
async def train_session_delete(request: Request):
    body = await request.json()
    name = body.get("name", "")
    if not name:
        raise HTTPException(400, "missing name")
    if recorder.delete_session(name):
        return {"ok": True}
    raise HTTPException(404, "session not found")


# ── Train Images ────────────────────────────────────────────────────
@router.get("/api/train/image")
async def train_image(session: str, index: int):
    if ".." in session or "/" in session:
        raise HTTPException(400, "invalid session name")
    path = os.path.join(config.record_dir, session, f"{index:04d}.jpg")
    if not os.path.exists(path):
        raise HTTPException(404, "image not found")
    return FileResponse(path, media_type="image/jpeg",
                        headers={"Cache-Control": "public, max-age=86400"})


# ── Train Embed ─────────────────────────────────────────────────────
@router.post("/api/train/embed")
async def train_embed(request: Request):
    body = await request.json()
    session = body.get("session", "")
    index = body.get("index", 0)
    if not session or not index:
        raise HTTPException(400, "missing session or index")

    # Read image file
    img_path = os.path.join(config.record_dir, session, f"{int(index):04d}.jpg")
    if not os.path.exists(img_path):
        raise HTTPException(404, "image not found")

    with open(img_path, "rb") as f:
        jpeg_bytes = f.read()

    emb = embed_image_bytes(jpeg_bytes)
    if emb is None:
        raise HTTPException(500, "embedding failed")

    # Load existing embeddings or create new
    embeddings = recorder.load_embeddings(session) or []

    # Read touch coordinates
    txt_path = os.path.join(config.record_dir, session, f"{int(index):04d}.txt")
    x, y = 0, 0
    if os.path.exists(txt_path):
        try:
            parts = open(txt_path).read().strip().split(",")
            x, y = int(parts[0]), int(parts[1])
        except (ValueError, IndexError):
            pass

    # Update or append
    found = False
    for e in embeddings:
        if e.get("index") == index:
            e["embedding"] = emb.tolist()
            e["x"] = x
            e["y"] = y
            found = True
            break
    if not found:
        embeddings.append({
            "index": index,
            "x": x,
            "y": y,
            "embedding": emb.tolist(),
        })

    recorder.save_embeddings(session, embeddings)
    return {"ok": True}


@router.get("/api/train/embeddings")
async def train_embeddings(session: str):
    data = recorder.load_embeddings(session)
    return data or []


# ── CLIP Load/Clear ─────────────────────────────────────────────────
@router.post("/api/clip/load")
async def clip_load(request: Request):
    body = await request.json()
    session = body.get("session", "")
    if not session:
        raise HTTPException(400, "missing session")
    data = recorder.load_embeddings(session)
    if not data:
        raise HTTPException(404, "no embeddings found")
    agent.clip_embeddings = data
    return {"ok": True, "count": len(data)}


@router.post("/api/clip/clear")
async def clip_clear():
    agent.clip_embeddings = None
    return {"ok": True}


# ── Hybrid Mode ────────────────────────────────────────────────────
@router.post("/api/hybrid/toggle")
async def hybrid_toggle(request: Request):
    body = await request.json()
    agent.hybrid_enabled = body.get("enabled", True)
    return {"ok": True, "hybrid_enabled": agent.hybrid_enabled}


@router.get("/api/memory/stats")
async def memory_stats():
    from scrcpy_ai.db.memory_manager import memory
    return memory.stats()


@router.post("/api/memory/clear-history")
async def memory_clear_history():
    agent.action_history.clear()
    agent._prev_phash = None
    agent._same_screen_count = 0
    return {"ok": True}


# ── Train Tree ──────────────────────────────────────────────────────
@router.get("/api/train/tree")
async def get_tree():
    return agent.train_tree or {"states": []}


@router.post("/api/train/tree")
async def post_tree(request: Request):
    body = await request.json()
    agent.train_tree = body
    return {"ok": True}
