"""
CHALO SAFAR AI - Vehicle Safety System
Laptop-side prediction + audio alerts through LAPTOP SPEAKERS

Flow:
  1. Connect to ESP32-CAM MJPEG stream
  2. Run ML model every 3 frames → Clear / Low Visibility
  3. If Low Visibility → fetch radar from ESP32
  4. Based on radar distance → speak one of 3 alerts:

     ALERT 1 (Caution)   → no obstacle / distance > 200 cm
       "Caution! Visibility is low, please stay alert."

     ALERT 2 (Warning)   → distance 80–200 cm
       "Warning! Obstacle detected in low visibility, please slow down."

     ALERT 3 (Emergency) → distance < 80 cm
       "Emergency! Stop immediately!"

Install requirements:
    pip install opencv-python tensorflow numpy requests pyttsx3
"""

import cv2
import numpy as np
import time
import requests
import threading
import pyttsx3
from tensorflow.keras.models import load_model

# ===========================
# CONFIG
# ===========================
ESP32_IP       = "192.168.137.253"       # ← Update this to your ESP32 IP
STREAM_URL     = f"http://{ESP32_IP}:81/stream"
RADAR_URL      = f"http://{ESP32_IP}/radar"

IMG_SIZE       = 224
CONFIDENCE_THR = 0.6     # below this → show "Detecting..."
MIN_ALERT_GAP  = 6.0     # seconds minimum between spoken alerts
RADAR_TIMEOUT  = 2.0     # seconds to wait for radar response

# Distance thresholds (centimeters)
DIST_EMERGENCY = 80      # < 80cm  → Emergency
DIST_WARNING   = 200     # < 200cm → Warning, else Caution

# ===========================
# ALERT MESSAGES (exact text)
# ===========================
ALERT_1 = "Caution! Visibility is low, please stay alert."
ALERT_2 = "Warning! Obstacle detected in low visibility, please slow down."
ALERT_3 = "Emergency! Stop immediately!"

class_names = ["Clear", "Low Visibility"]

# ===========================
# TTS SETUP
# ===========================
is_speaking = False
tts_lock    = threading.Lock()

def speak(text):
    """Speak text on laptop speakers in a background thread."""
    global is_speaking

    def _run():
        global is_speaking
        with tts_lock:
            is_speaking = True
            try:
                engine = pyttsx3.init()
                engine.setProperty("rate",   155)   # speed
                engine.setProperty("volume", 1.0)   # max volume
                # Pick English voice if available
                for v in engine.getProperty("voices"):
                    if "english" in v.name.lower() or "zira" in v.name.lower() \
                            or "david" in v.name.lower() or "en_" in v.id.lower():
                        engine.setProperty("voice", v.id)
                        break
                engine.say(text)
                engine.runAndWait()
            except Exception as e:
                print(f"[TTS] Error: {e}")
            finally:
                is_speaking = False

    threading.Thread(target=_run, daemon=True).start()

# ===========================
# RADAR FETCH
# ===========================
def fetch_radar():
    """GET /radar from ESP32. Returns JSON dict or None."""
    try:
        r = requests.get(RADAR_URL, timeout=RADAR_TIMEOUT)
        if r.status_code == 200:
            return r.json()
    except Exception as e:
        print(f"[RADAR] Fetch error: {e}")
    return None

# ===========================
# PICK ALERT FROM RADAR DATA
# ===========================
def pick_alert(radar):
    """
    Returns (alert_message, risk_label, display_distance_str)
    Defaults to ALERT_1 if no obstacle or radar unreachable.
    """
    if radar is None or not radar.get("target_detected", False):
        return ALERT_1, "Caution", "No obstacle detected"

    dist = radar.get("detection_distance_cm", 9999)

    if dist < DIST_EMERGENCY:
        return ALERT_3, "EMERGENCY", f"Obstacle at {dist} cm"
    elif dist < DIST_WARNING:
        return ALERT_2, "WARNING",   f"Obstacle at {dist} cm"
    else:
        return ALERT_1, "Caution",   f"Obstacle at {dist} cm"

# ===========================
# LOAD MODEL
# ===========================
print("⏳ Loading visibility model...")
model = load_model("visibility_model.keras")
print("✅ Model loaded!\n")

# ===========================
# STATE
# ===========================
last_alert_time = 0
display_text    = "Starting..."
display_color   = (255, 255, 255)
risk_display    = ""
dist_display    = ""

# ===========================
# MAIN LOOP
# ===========================
print(f"⏳ Connecting to: {STREAM_URL}\n")

while True:
    cap = cv2.VideoCapture(STREAM_URL)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    if not cap.isOpened():
        print("❌ Cannot connect to stream. Retrying in 2s...")
        time.sleep(2)
        continue

    print("✅ Stream connected!\n")
    frame_count = 0

    while True:
        ret, frame = cap.read()
        if not ret:
            print("⚠️  Stream dropped. Reconnecting...")
            break

        frame_count += 1

        # ── ML inference every 3 frames ──────────────────────────
        if frame_count % 3 == 0:
            img  = cv2.resize(frame, (IMG_SIZE, IMG_SIZE))
            img  = img / 255.0
            img  = np.reshape(img, (1, IMG_SIZE, IMG_SIZE, 3))
            pred = model.predict(img, verbose=0)

            confidence = float(np.max(pred))
            label_idx  = int(np.argmax(pred))

            # ── Below confidence threshold ──
            if confidence < CONFIDENCE_THR:
                display_text  = "Detecting..."
                display_color = (0, 220, 220)
                risk_display  = ""
                dist_display  = ""

            # ── CLEAR ──
            elif class_names[label_idx] == "Clear":
                display_text  = f"Clear  ({confidence:.0%})"
                display_color = (0, 210, 0)
                risk_display  = ""
                dist_display  = ""

            # ── LOW VISIBILITY ──
            else:
                display_text  = f"Low Visibility  ({confidence:.0%})"
                display_color = (0, 0, 255)

                now = time.time()

                # Fire alert if not already speaking and gap has passed
                if not is_speaking and (now - last_alert_time >= MIN_ALERT_GAP):
                    radar = fetch_radar()
                    alert_msg, risk_label, dist_str = pick_alert(radar)

                    risk_display = risk_label
                    dist_display = dist_str

                    print(f"[ALERT] {risk_label} — {dist_str}")
                    print(f"        Speaking: \"{alert_msg}\"")
                    speak(alert_msg)
                    last_alert_time = now

        # ── Draw HUD ─────────────────────────────────────────────
        h, w = frame.shape[:2]

        # Dark top banner
        banner = frame.copy()
        cv2.rectangle(banner, (0, 0), (w, 90), (10, 10, 10), -1)
        cv2.addWeighted(banner, 0.5, frame, 0.5, 0, frame)

        # Line 1 — visibility status
        cv2.putText(frame, display_text, (10, 35),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.85, display_color, 2)

        # Line 2 — risk + distance
        if risk_display:
            if risk_display == "EMERGENCY":
                rc = (0, 0, 255)
            elif risk_display == "WARNING":
                rc = (0, 80, 255)
            else:
                rc = (0, 200, 255)

            line2 = f"{risk_display}  |  {dist_display}"
            cv2.putText(frame, line2, (10, 72),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.72, rc, 2)

        # Bottom — next alert countdown
        elapsed   = time.time() - last_alert_time
        remaining = max(0.0, MIN_ALERT_GAP - elapsed)
        if remaining > 0:
            bottom_txt = f"Next alert in {remaining:.1f}s"
        else:
            bottom_txt = "Alert ready"
        cv2.putText(frame, bottom_txt, (10, h - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.48, (140, 140, 140), 1)

        cv2.imshow("CHALO SAFAR AI", frame)

        if cv2.waitKey(1) & 0xFF == 27:   # ESC to quit
            cap.release()
            cv2.destroyAllWindows()
            print("\n👋 Exiting CHALO SAFAR AI.")
            exit()

    cap.release()