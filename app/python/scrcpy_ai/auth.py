"""TOTP authentication for external access."""

import hashlib
import json
import logging
import os
import secrets
import time

import pyotp

from scrcpy_ai.config import config

logger = logging.getLogger(__name__)

# Session store: {token: expiry_timestamp}
_sessions: dict[str, float] = {}

SESSION_TTL = 2 * 60 * 60  # 2 hours (must match cookie max_age in web/routes.py)


def _sessions_path() -> str:
    return os.path.join(config.db_dir, "sessions.json")


def _save_sessions() -> None:
    """Persist sessions so a scrcpy-ai restart doesn't force everyone to re-login."""
    try:
        os.makedirs(config.db_dir, exist_ok=True)
        path = _sessions_path()
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(_sessions, f)
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except OSError:
        logger.warning("could not persist sessions", exc_info=True)


def _load_sessions() -> None:
    """Restore non-expired sessions from disk at startup."""
    try:
        with open(_sessions_path()) as f:
            data = json.load(f)
        now = time.time()
        _sessions.update({t: e for t, e in data.items() if isinstance(e, (int, float)) and e > now})
    except (FileNotFoundError, ValueError, OSError):
        pass


def _secret_path() -> str:
    return os.path.join(config.db_dir, "totp_secret.key")


def get_or_create_secret() -> str:
    """Get existing TOTP secret or create a new one."""
    os.makedirs(config.db_dir, exist_ok=True)
    path = _secret_path()
    if os.path.exists(path):
        with open(path) as f:
            return f.read().strip()
    secret = pyotp.random_base32()
    with open(path, "w") as f:
        f.write(secret)
    os.chmod(path, 0o600)
    logger.info("New TOTP secret generated: %s", path)
    return secret


def get_totp() -> pyotp.TOTP:
    return pyotp.TOTP(get_or_create_secret())


def get_provisioning_uri() -> str:
    """Get URI for QR code (Google Authenticator registration)."""
    totp = get_totp()
    return totp.provisioning_uri(name="scrcpy-ai", issuer_name="jhbot")


def verify_otp(code: str) -> bool:
    """Verify a TOTP code (allows 1 step tolerance for clock drift)."""
    totp = get_totp()
    return totp.verify(code, valid_window=1)


def create_session() -> str:
    """Create a new session token."""
    _cleanup_expired()
    token = secrets.token_urlsafe(32)
    _sessions[token] = time.time() + SESSION_TTL
    _save_sessions()
    return token


def validate_session(token: str) -> bool:
    """Check if session token is valid and refresh expiry."""
    if not token:
        return False
    expiry = _sessions.get(token)
    if not expiry:
        return False
    if time.time() > expiry:
        _sessions.pop(token, None)
        return False
    # Refresh TTL on activity
    _sessions[token] = time.time() + SESSION_TTL
    return True


def _cleanup_expired():
    """Remove expired sessions."""
    now = time.time()
    expired = [k for k, v in _sessions.items() if now > v]
    for k in expired:
        _sessions.pop(k, None)
    if expired:
        _save_sessions()


# Restore sessions saved before the last restart (module import time).
_load_sessions()


def is_internal_request(client_host: str, forwarded_for: str | None,
                        host_header: str | None = None) -> bool:
    """Check if request originates from localhost (not proxied external).

    Key insight: Apache proxy connects from 127.0.0.1, so client_host alone
    is unreliable. Use multiple signals:
    - X-Forwarded-For present → proxied → external
    - Host header is external domain → external
    - Only truly internal if direct localhost with no proxy indicators
    """
    # If X-Forwarded-For is set, request came through a proxy
    if forwarded_for:
        return False

    # If Host header is an external domain, it's external
    if host_header:
        h = host_header.split(":")[0].lower()
        if h not in ("127.0.0.1", "::1", "localhost"):
            return False

    # Direct connection from localhost with no proxy indicators
    return client_host in ("127.0.0.1", "::1", "localhost")
