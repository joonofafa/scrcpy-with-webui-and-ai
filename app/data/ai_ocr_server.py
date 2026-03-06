#!/usr/bin/env python3
"""
PaddleOCR daemon for scrcpy AI agent.

Protocol (stdin/stdout, binary):
  - Each message: 4-byte big-endian length + payload
  - Request payload: raw JPEG bytes
  - Response payload: JSON string (UTF-8)
  - Sentinel: length == 0 → graceful shutdown

Startup sequence:
  1. Sends {"status":"ready"} immediately
  2. Loads PaddleOCR model
  3. Sends {"status":"loaded"}
  4. Enters request loop
"""

import json
import os
import struct
import sys
import io

# Suppress PaddleOCR/PaddlePaddle verbose stderr output
# This prevents the child process from blocking on a full stderr pipe
sys.stderr = open(os.devnull, 'w')
os.environ['GLOG_minloglevel'] = '3'
os.environ['PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK'] = 'True'


def send_msg(data: bytes):
    """Send a length-prefixed message to stdout."""
    sys.stdout.buffer.write(struct.pack('>I', len(data)))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def recv_msg() -> bytes | None:
    """Receive a length-prefixed message from stdin. Returns None on sentinel."""
    header = sys.stdin.buffer.read(4)
    if len(header) < 4:
        return None
    length = struct.unpack('>I', header)[0]
    if length == 0:
        return None
    data = b''
    while len(data) < length:
        chunk = sys.stdin.buffer.read(length - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def main():
    # Signal ready before loading (model load takes seconds)
    send_msg(json.dumps({"status": "ready"}).encode('utf-8'))

    try:
        from paddleocr import PaddleOCR
        from PIL import Image
    except ImportError as e:
        send_msg(json.dumps({
            "status": "error",
            "message": f"Missing dependency: {e}"
        }).encode('utf-8'))
        return

    # Load model (CPU mode, Korean + English)
    ocr = PaddleOCR(
        lang='korean',
        use_angle_cls=True,
        use_gpu=False,
        show_log=False,
    )

    send_msg(json.dumps({"status": "loaded"}).encode('utf-8'))

    # Request loop
    while True:
        jpeg_data = recv_msg()
        if jpeg_data is None:
            break

        try:
            img = Image.open(io.BytesIO(jpeg_data)).convert('RGB')
            import numpy as np
            import cv2

            img_array = np.array(img)

            # Preprocess: grayscale → contrast enhance → adaptive binarize
            gray = cv2.cvtColor(img_array, cv2.COLOR_RGB2GRAY)
            # CLAHE for local contrast enhancement
            clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
            enhanced = clahe.apply(gray)
            # Adaptive threshold for clean text edges
            binary = cv2.adaptiveThreshold(
                enhanced, 255,
                cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                cv2.THRESH_BINARY,
                blockSize=31, C=10,
            )
            # Convert back to 3-channel for PaddleOCR
            preprocessed = cv2.cvtColor(binary, cv2.COLOR_GRAY2RGB)

            # Run OCR on both original and preprocessed, merge results
            all_texts = {}  # key: (cx, cy) rounded to 20px grid

            for src in [img_array, preprocessed]:
                result = ocr.ocr(src, cls=True)
                if result and result[0]:
                    for line in result[0]:
                        bbox, (text, conf) = line
                        xs = [p[0] for p in bbox]
                        ys = [p[1] for p in bbox]
                        cx = int((min(xs) + max(xs)) / 2)
                        cy = int((min(ys) + max(ys)) / 2)
                        # Grid key to deduplicate nearby detections
                        key = (cx // 20, cy // 20)
                        if key not in all_texts or conf > all_texts[key]["conf"]:
                            all_texts[key] = {
                                "text": text,
                                "center": [cx, cy],
                                "conf": round(float(conf), 3),
                            }

            texts = list(all_texts.values())

            response = json.dumps({"texts": texts}, ensure_ascii=False)
            send_msg(response.encode('utf-8'))

        except Exception as e:
            error_resp = json.dumps({"error": str(e)})
            send_msg(error_resp.encode('utf-8'))


if __name__ == '__main__':
    main()
