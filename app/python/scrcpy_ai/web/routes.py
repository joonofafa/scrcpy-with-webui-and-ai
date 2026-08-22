"""FastAPI routes — authentication endpoints for external (TOTP) access.

The web UI is a pure remote-control client: video and touch/key control flow
over the /ws/video and /ws/control WebSocket proxies (see main.py), so the only
HTTP API left here is the TOTP login/setup used to gate external access.
"""

import base64
import io
import logging

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import JSONResponse

from scrcpy_ai.auth import (
    create_session,
    get_or_create_secret,
    get_provisioning_uri,
    is_internal_request,
    verify_otp,
)

logger = logging.getLogger(__name__)
router = APIRouter()


# ── Auth ───────────────────────────────────────────────────────────
@router.post("/auth/login")
async def auth_login(request: Request):
    body = await request.json()
    code = body.get("code", "")
    if not verify_otp(code):
        return JSONResponse({"ok": False, "error": "인증 실패"}, status_code=401)
    token = create_session()
    response = JSONResponse({"ok": True})
    # Apache proxies HTTPS→HTTP internally, so check X-Forwarded-Proto
    is_https = request.headers.get("x-forwarded-proto") == "https"
    response.set_cookie(
        key="session",
        value=token,
        httponly=True,
        secure=is_https,
        samesite="lax",
        max_age=2 * 60 * 60,  # 2 hours (must match auth.SESSION_TTL)
    )
    return response


@router.get("/auth/setup")
async def auth_setup(request: Request):
    """QR code setup — internal access only."""
    client_host = request.client.host if request.client else ""
    forwarded_for = request.headers.get("x-forwarded-for")
    host_header = request.headers.get("host")
    if not is_internal_request(client_host, forwarded_for, host_header):
        raise HTTPException(403, "internal access only")

    import qrcode

    uri = get_provisioning_uri()
    secret = get_or_create_secret()

    img = qrcode.make(uri)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    qr_b64 = base64.b64encode(buf.getvalue()).decode("ascii")

    return {"qr_base64": qr_b64, "secret": secret}
