"""Recording sessions: capture screenshots + touch coordinates."""

import json
import logging
import os
import time

from scrcpy_ai.config import config
from scrcpy_ai.device import client as device

logger = logging.getLogger(__name__)


def get_session_dir() -> str:
    """Get or create the current recording session directory."""
    ts = time.strftime("%Y%m%d_%H%M%S")
    path = os.path.join(config.record_dir, ts)
    os.makedirs(path, exist_ok=True)
    return path


def list_sessions() -> list[dict]:
    """List all recording sessions."""
    sessions = []
    base = config.record_dir
    if not os.path.isdir(base):
        return sessions
    for name in sorted(os.listdir(base), reverse=True):
        path = os.path.join(base, name)
        if not os.path.isdir(path):
            continue
        # Count captures
        count = len([f for f in os.listdir(path) if f.endswith(".jpg")])
        sessions.append({"name": name, "count": count})
    return sessions


def get_session(name: str) -> list[dict]:
    """Get captures for a session."""
    path = os.path.join(config.record_dir, name)
    if not os.path.isdir(path):
        return []

    captures = []
    for fname in os.listdir(path):
        if not fname.endswith(".jpg"):
            continue
        try:
            idx = int(fname.replace(".jpg", ""))
        except ValueError:
            continue
        txt = os.path.join(path, f"{idx:04d}.txt")
        x, y = 0, 0
        if os.path.exists(txt):
            try:
                data = open(txt).read().strip()
                parts = data.split(",")
                if len(parts) >= 2:
                    x, y = int(parts[0]), int(parts[1])
            except (ValueError, IndexError):
                pass
        captures.append({"index": idx, "x": x, "y": y})

    captures.sort(key=lambda c: c["index"])
    return captures


def delete_session(name: str) -> bool:
    """Delete a recording session."""
    import shutil
    # Path traversal protection
    if "/" in name or "\\" in name or ".." in name:
        return False
    path = os.path.join(config.record_dir, name)
    if os.path.isdir(path):
        shutil.rmtree(path)
        return True
    return False


def save_capture(session_dir: str, index: int, jpeg_bytes: bytes, x: int, y: int):
    """Save a capture (screenshot + touch coords) to session directory."""
    jpg_path = os.path.join(session_dir, f"{index:04d}.jpg")
    txt_path = os.path.join(session_dir, f"{index:04d}.txt")
    with open(jpg_path, "wb") as f:
        f.write(jpeg_bytes)
    with open(txt_path, "w") as f:
        f.write(f"{x},{y}")


def load_embeddings(session_name: str) -> list[dict] | None:
    """Load embeddings.json from a session directory."""
    path = os.path.join(config.record_dir, session_name, "embeddings.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return None


def save_embeddings(session_name: str, embeddings: list[dict]):
    """Save embeddings.json to a session directory."""
    path = os.path.join(config.record_dir, session_name, "embeddings.json")
    with open(path, "w") as f:
        json.dump(embeddings, f)
