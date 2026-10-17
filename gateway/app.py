"""StackChan Local LLM Gateway (Android Termux / FastAPI).

ブラウザから質問を受け取り、ローカル LLM (llama.cpp llama-server) で回答を生成し、
espeak-ng で WAV 音声を生成して StackChan に喋らせる中継サーバ。

- LLM:        http://127.0.0.1:8080/v1/chat/completions (llama-server)
- listen:     0.0.0.0:50021
- StackChan:  POST http://{stackchan_host}:{stackchan_port}/api/speak

音声認識(STT)は扱わない。詳細仕様は リポジトリ直下の CLAUDE.md を参照。
"""

from __future__ import annotations

import datetime
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import threading
from pathlib import Path
from typing import Any, Dict, List, Optional

import requests
from fastapi import FastAPI, Form, Request, Response
from fastapi.responses import (
    FileResponse,
    HTMLResponse,
    JSONResponse,
    RedirectResponse,
)

# ----------------------------------------------------------------------------
# 定数・パス
# ----------------------------------------------------------------------------

BASE_DIR = Path(os.path.expanduser("~/stackchan_gateway"))
HISTORY_DIR = BASE_DIR / "history"
LOGS_DIR = BASE_DIR / "logs"
SETTINGS_PATH = BASE_DIR / "settings.json"
CURRENT_JSON = BASE_DIR / "current.json"
CURRENT_WAV = BASE_DIR / "current.wav"
SAY_WAV = BASE_DIR / "say.wav"

# StackChan のプリンタ実況: /synthesis 本文がこの接頭辞で始まれば、続く文章をそのまま読み上げる
SAY_PREFIX = "__SAY__"
_say_lock = threading.Lock()

LLM_URL = "http://127.0.0.1:8080/v1/chat/completions"
LLM_HEALTH_URL = "http://127.0.0.1:8080/health"
LLM_MODELS_URL = "http://127.0.0.1:8080/v1/models"
LLM_MODEL_ALIAS = "local"

COOKIE_NAME = "sc_session"

# 設定のデフォルト値（password_* / session_secret は初回起動時に生成）
DEFAULT_SETTINGS: Dict[str, Any] = {
    "password_hash": "",
    "password_salt": "",
    "session_secret": "",
    "stackchan_host": "",
    "stackchan_port": 80,
    "voice_lang": "ja",
    "tts_voice": "default",
    "pitch": 99,
    "volume": 200,
    "speed": 150,
    "answer_mode": "short",
    "kanji_to_kana": True,
    "auto_speak": True,
    "max_history": 50,
    "system_prompt": (
        "あなたはStackChanです。ユーザーの質問に短く自然に答えてください。"
        "音声で読み上げやすいように、長すぎる文、箇条書き、記号の多用は避けてください。"
    ),
}

INITIAL_PASSWORD = "stackchan"

# 回答長モード → (日本語指示, 英語指示, max_tokens)
ANSWER_MODES = {
    "hitokoto": ("一言で、できるだけ短く答えてください。", "Answer in just a few words.", 64),
    "short": ("短く簡潔に答えてください。", "Answer briefly.", 160),
    "normal": ("普通の長さで自然に答えてください。", "Answer naturally.", 384),
}

# espeak-ng の声色バリアント。言語は voice_lang で決め、ここでは音色だけ変える。
TTS_VOICE_VARIANTS = {
    "default": "",
    "f1": "+f1",
    "f2": "+f2",
    "m1": "+m1",
    "m2": "+m2",
    "m3": "+m3",
}

# ----------------------------------------------------------------------------
# pykakasi（漢字→かな変換）。未インストールでも動作するようフォールバックする。
# ----------------------------------------------------------------------------

try:
    import pykakasi  # type: ignore

    _kks = pykakasi.kakasi()
except Exception:  # pragma: no cover - 環境依存
    _kks = None


def to_hiragana(text: str) -> str:
    """漢字混じり文をひらがな読みに変換する（pykakasi が無ければ原文を返す）。"""
    if not _kks:
        return text
    try:
        parts = _kks.convert(text)
        return "".join(p.get("hira", "") for p in parts)
    except Exception:
        return text


# ----------------------------------------------------------------------------
# 設定の読み書き
# ----------------------------------------------------------------------------


def ensure_dirs() -> None:
    BASE_DIR.mkdir(parents=True, exist_ok=True)
    HISTORY_DIR.mkdir(parents=True, exist_ok=True)
    LOGS_DIR.mkdir(parents=True, exist_ok=True)


def hash_password(password: str, salt: str) -> str:
    """salt + sha256 でパスワードをハッシュ化する（平文保存しない）。"""
    return hashlib.sha256((salt + password).encode("utf-8")).hexdigest()


def load_settings() -> Dict[str, Any]:
    """settings.json を読み込む。無ければ初期パスワードで生成する。"""
    ensure_dirs()
    settings = dict(DEFAULT_SETTINGS)
    if SETTINGS_PATH.exists():
        try:
            stored = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            settings.update(stored)
        except Exception:
            pass

    changed = False
    if not settings.get("password_salt") or not settings.get("password_hash"):
        salt = secrets.token_hex(16)
        settings["password_salt"] = salt
        settings["password_hash"] = hash_password(INITIAL_PASSWORD, salt)
        changed = True
    if not settings.get("session_secret"):
        settings["session_secret"] = secrets.token_hex(32)
        changed = True
    if changed:
        save_settings(settings)
    return settings


def save_settings(settings: Dict[str, Any]) -> None:
    ensure_dirs()
    tmp = SETTINGS_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(SETTINGS_PATH)


# ----------------------------------------------------------------------------
# セッション（署名付きCookie・単一ユーザ）
# ----------------------------------------------------------------------------


def session_token(secret: str) -> str:
    return hmac.new(secret.encode(), b"stackchan-gateway", hashlib.sha256).hexdigest()


def is_logged_in(request: Request, settings: Dict[str, Any]) -> bool:
    token = request.cookies.get(COOKIE_NAME, "")
    if not token:
        return False
    return hmac.compare_digest(token, session_token(settings["session_secret"]))


# ----------------------------------------------------------------------------
# 値のクランプ・サニタイズ
# ----------------------------------------------------------------------------


def clamp(value: Any, low: int, high: int, default: int) -> int:
    try:
        v = int(value)
    except (TypeError, ValueError):
        return default
    return max(low, min(high, v))


def norm_lang(value: Any) -> str:
    return "en" if str(value).lower().startswith("en") else "ja"


def norm_mode(value: Any) -> str:
    return value if value in ANSWER_MODES else "short"


def norm_tts_voice(value: Any) -> str:
    voice = str(value or "").strip().lower()
    return voice if voice in TTS_VOICE_VARIANTS else "default"


def japanese_char_count(text: str) -> int:
    count = 0
    for ch in text:
        code = ord(ch)
        if (
            0x3040 <= code <= 0x30FF  # Hiragana / Katakana
            or 0x3400 <= code <= 0x9FFF  # CJK ideographs
            or 0xF900 <= code <= 0xFAFF
        ):
            count += 1
    return count


def latin_char_count(text: str) -> int:
    return sum(1 for ch in text if ("a" <= ch.lower() <= "z"))


def speech_lang_for(text: str, requested_lang: Any) -> str:
    lang = norm_lang(requested_lang)
    if lang == "en":
        ja_chars = japanese_char_count(text)
        if ja_chars >= 3 and ja_chars >= latin_char_count(text):
            return "ja"
    return lang


def espeak_voice(lang: str, tts_voice: Any) -> str:
    base = "ja" if norm_lang(lang) == "ja" else "en-us"
    return base + TTS_VOICE_VARIANTS[norm_tts_voice(tts_voice)]


# ----------------------------------------------------------------------------
# LLM 呼び出し
# ----------------------------------------------------------------------------


def llm_reachable() -> bool:
    for url in (LLM_HEALTH_URL, LLM_MODELS_URL):
        try:
            r = requests.get(url, timeout=2.5)
            if r.status_code == 200:
                return True
        except requests.RequestException:
            continue
    return False


def call_llm(question: str, settings: Dict[str, Any], lang: str, mode: str) -> str:
    """llama-server にチャット補完を要求し、回答テキストを返す。"""
    ja_inst, en_inst, max_tokens = ANSWER_MODES[mode]
    length_inst = ja_inst if lang == "ja" else en_inst
    lang_inst = (
        "日本語で答えてください。" if lang == "ja" else "Answer in English."
    )
    system_prompt = f"{settings['system_prompt']}\n{lang_inst}\n{length_inst}"

    payload = {
        "model": LLM_MODEL_ALIAS,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": question},
        ],
        "temperature": 0.7,
        "max_tokens": max_tokens,
        "stream": False,
    }
    r = requests.post(LLM_URL, json=payload, timeout=120)
    r.raise_for_status()
    data = r.json()
    answer = data["choices"][0]["message"]["content"].strip()
    return answer


# ----------------------------------------------------------------------------
# 音声生成（espeak-ng）
# ----------------------------------------------------------------------------


# マークダウン/装飾記号を除去する正規表現（espeak がそのまま読み上げるのを防ぐ）
_MD_SYMBOLS = re.compile(r"[*#`_~>|]")
_MD_LINK = re.compile(r"\[(.*?)\]\((?:.*?)\)")
_MULTISPACE = re.compile(r"[ \t]+")
_NEWLINES = re.compile(r"\s*\n+\s*")


def sanitize_for_speech(text: str) -> str:
    """読み上げ用にテキストを整形する。
    - マークダウン記号（*, #, `, [..](..) 等）を除去
    - 箇条書きの「・」を読点に、改行を句点に変換
    - 連続空白を 1 つに
    espeak はこれらの記号を「アスタリスク」等と読んでしまうため事前に除去する。
    """
    text = _MD_LINK.sub(r"\1", text)
    text = _MD_SYMBOLS.sub("", text)
    text = text.replace("・", "、")
    text = _NEWLINES.sub("。", text)
    text = _MULTISPACE.sub(" ", text)
    return text.strip()


def make_speech_text(answer: str, lang: str, kanji_to_kana: bool) -> str:
    """発話用テキストを作る。記号除去 → ja かつ漢字読み変換ONなら かな に変換する。"""
    cleaned = sanitize_for_speech(answer)
    if lang == "ja" and kanji_to_kana:
        return to_hiragana(cleaned)
    return cleaned


def synth_wav(
    text: str,
    lang: str,
    pitch: int,
    volume: int,
    speed: int,
    out_path: Path,
    tts_voice: Any = "default",
) -> None:
    """espeak-ng で WAV を生成する。"""
    voice = espeak_voice(lang, tts_voice)
    if not text.strip():
        text = "..."
    cmd = [
        "espeak-ng",
        "-v", voice,
        "-p", str(clamp(pitch, 0, 99, 80)),
        "-a", str(clamp(volume, 0, 200, 200)),
        "-s", str(clamp(speed, 80, 260, 150)),
        "-w", str(out_path),
        text,
    ]
    subprocess.run(cmd, check=True, capture_output=True)


def synth_say_wav(text: str, settings: Dict[str, Any]) -> Optional[Path]:
    """実況テキストを現在の音声設定で WAV にする（LLM は使わない）。失敗時は None。"""
    text = text.strip()
    if not text:
        return None
    lang = speech_lang_for(text, settings.get("voice_lang", "ja"))
    speech_text = make_speech_text(text, lang, bool(settings.get("kanji_to_kana", True)))
    with _say_lock:
        try:
            synth_wav(
                speech_text,
                lang,
                clamp(settings.get("pitch"), 0, 99, 80),
                clamp(settings.get("volume"), 0, 200, 200),
                clamp(settings.get("speed"), 80, 260, 150),
                SAY_WAV,
                settings.get("tts_voice", "default"),
            )
        except Exception:
            return None
    return SAY_WAV


def ensure_default_wav() -> Path:
    """current.wav が無いとき用のデフォルト音声を生成して返す。"""
    default_path = BASE_DIR / "default.wav"
    if not default_path.exists():
        synth_wav("まだ回答がありません。", "ja", 80, 200, 150, default_path)
    return default_path


# ----------------------------------------------------------------------------
# 履歴・current 管理
# ----------------------------------------------------------------------------


def new_history_id() -> str:
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"{ts}_{secrets.token_hex(2)}"


def history_json_path(item_id: str) -> Path:
    return HISTORY_DIR / f"{item_id}.json"


def history_wav_path(item_id: str) -> Path:
    return HISTORY_DIR / f"{item_id}.wav"


def build_item(
    question: str,
    answer: str,
    speech_text: str,
    lang: str,
    pitch: int,
    volume: int,
    speed: int,
    mode: str,
    tts_voice: str = "default",
    speech_lang: Optional[str] = None,
    item_id: Optional[str] = None,
) -> Dict[str, Any]:
    item_id = item_id or new_history_id()
    return {
        "id": item_id,
        "created_at": datetime.datetime.now().astimezone().isoformat(),
        "question": question,
        "answer": answer,
        "speech_text": speech_text,
        "voice_lang": lang,
        "speech_lang": speech_lang or lang,
        "tts_voice": norm_tts_voice(tts_voice),
        "pitch": pitch,
        "volume": volume,
        "speed": speed,
        "answer_mode": mode,
        "wav_file": f"{item_id}.wav",
    }


def save_history_item(item: Dict[str, Any]) -> None:
    history_json_path(item["id"]).write_text(
        json.dumps(item, ensure_ascii=False, indent=2), encoding="utf-8"
    )


def load_history_item(item_id: str) -> Optional[Dict[str, Any]]:
    path = history_json_path(item_id)
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def list_history(max_items: int = 200) -> List[Dict[str, Any]]:
    items: List[Dict[str, Any]] = []
    for path in HISTORY_DIR.glob("*.json"):
        try:
            items.append(json.loads(path.read_text(encoding="utf-8")))
        except Exception:
            continue
    items.sort(key=lambda x: x.get("id", ""), reverse=True)
    return items[:max_items]


def prune_history(max_history: int) -> None:
    """max_history を超える古い履歴（json+wav）を削除する。"""
    items = list_history(max_items=100000)
    for item in items[max_history:]:
        history_json_path(item["id"]).unlink(missing_ok=True)
        history_wav_path(item["id"]).unlink(missing_ok=True)


def set_current(item: Dict[str, Any]) -> None:
    """履歴 item を current として設定する（current.json/current.wav を更新）。"""
    wav = history_wav_path(item["id"])
    if wav.exists():
        shutil.copyfile(wav, CURRENT_WAV)
    CURRENT_JSON.write_text(
        json.dumps(item, ensure_ascii=False, indent=2), encoding="utf-8"
    )


def load_current() -> Optional[Dict[str, Any]]:
    if not CURRENT_JSON.exists():
        return None
    try:
        return json.loads(CURRENT_JSON.read_text(encoding="utf-8"))
    except Exception:
        return None


# ----------------------------------------------------------------------------
# StackChan 連携
# ----------------------------------------------------------------------------


def stackchan_base(settings: Dict[str, Any]) -> Optional[str]:
    host = (settings.get("stackchan_host") or "").strip()
    if not host:
        return None
    port = settings.get("stackchan_port", 80)
    return f"http://{host}:{port}"


def stackchan_reachable(settings: Dict[str, Any]) -> bool:
    base = stackchan_base(settings)
    if not base:
        return False
    try:
        r = requests.get(f"{base}/api/status", timeout=2.5)
        return r.status_code == 200
    except requests.RequestException:
        return False


def call_stackchan_speak(settings: Dict[str, Any], text: str = "__CURRENT__") -> Dict[str, Any]:
    """StackChan の POST /api/speak を呼ぶ。"""
    base = stackchan_base(settings)
    if not base:
        return {"ok": False, "error": "stackchan_host not set"}
    try:
        r = requests.post(f"{base}/api/speak", data={"text": text}, timeout=5)
        try:
            return r.json()
        except ValueError:
            return {"ok": r.status_code == 200, "status_code": r.status_code}
    except requests.RequestException as exc:
        return {"ok": False, "error": str(exc)}


# ----------------------------------------------------------------------------
# ネットワーク情報
# ----------------------------------------------------------------------------


def local_ip_candidates() -> List[str]:
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None):
            ip = info[4][0]
            if "." in ip and not ip.startswith("127."):
                ips.add(ip)
    except OSError:
        pass
    return sorted(ips)


# ----------------------------------------------------------------------------
# 質問処理パイプライン（LLM → 音声生成 → current/history 保存）
# ----------------------------------------------------------------------------


def process_question(question: str, settings: Dict[str, Any], lang: str, mode: str) -> Dict[str, Any]:
    """質問を LLM 処理し、音声生成して current/history に保存した item を返す。

    例外（LLM失敗・TTS失敗）は呼び出し側で処理する。
    """
    answer = call_llm(question, settings, lang, mode)
    speech_lang = speech_lang_for(answer, lang)
    speech_text = make_speech_text(answer, speech_lang, bool(settings.get("kanji_to_kana", True)))

    pitch = clamp(settings.get("pitch"), 0, 99, 80)
    volume = clamp(settings.get("volume"), 0, 200, 200)
    speed = clamp(settings.get("speed"), 80, 260, 150)
    tts_voice = norm_tts_voice(settings.get("tts_voice", "default"))

    item = build_item(
        question,
        answer,
        speech_text,
        lang,
        pitch,
        volume,
        speed,
        mode,
        tts_voice=tts_voice,
        speech_lang=speech_lang,
    )
    synth_wav(speech_text, speech_lang, pitch, volume, speed, history_wav_path(item["id"]), tts_voice)
    save_history_item(item)
    set_current(item)
    prune_history(clamp(settings.get("max_history"), 1, 100000, 50))
    return item


def revoice_item(item: Dict[str, Any], settings: Dict[str, Any], overrides: Dict[str, Any]) -> Dict[str, Any]:
    """既存 item の回答テキストから音声だけ再生成する（LLM は呼ばない）。"""
    lang = norm_lang(overrides.get("voice_lang", item.get("voice_lang", "ja")))
    speech_lang = speech_lang_for(item.get("answer", ""), lang)
    pitch = clamp(overrides.get("pitch", item.get("pitch")), 0, 99, 80)
    volume = clamp(overrides.get("volume", item.get("volume")), 0, 200, 200)
    speed = clamp(overrides.get("speed", item.get("speed")), 80, 260, 150)
    tts_voice = norm_tts_voice(
        overrides.get("tts_voice", item.get("tts_voice", settings.get("tts_voice", "default")))
    )
    kanji_to_kana = overrides.get("kanji_to_kana")
    if kanji_to_kana is None:
        kanji_to_kana = bool(settings.get("kanji_to_kana", True))

    speech_text = make_speech_text(item["answer"], speech_lang, bool(kanji_to_kana))
    item.update(
        {
            "speech_text": speech_text,
            "voice_lang": lang,
            "speech_lang": speech_lang,
            "tts_voice": tts_voice,
            "pitch": pitch,
            "volume": volume,
            "speed": speed,
        }
    )
    synth_wav(speech_text, speech_lang, pitch, volume, speed, history_wav_path(item["id"]), tts_voice)
    save_history_item(item)
    return item


# ----------------------------------------------------------------------------
# FastAPI アプリ
# ----------------------------------------------------------------------------

app = FastAPI(title="StackChan Gateway")


def require_login(request: Request) -> Optional[Dict[str, Any]]:
    settings = load_settings()
    if not is_logged_in(request, settings):
        return None
    return settings


# --- 認証 -------------------------------------------------------------------


@app.get("/", response_class=HTMLResponse)
def index(request: Request) -> HTMLResponse:
    settings = load_settings()
    if not is_logged_in(request, settings):
        return HTMLResponse(LOGIN_HTML)
    return HTMLResponse(MAIN_HTML)


@app.post("/login")
def login(request: Request, password: str = Form("")) -> Response:
    settings = load_settings()
    expected = settings["password_hash"]
    given = hash_password(password, settings["password_salt"])
    if not hmac.compare_digest(given, expected):
        return HTMLResponse(LOGIN_HTML.replace("<!--ERR-->", "パスワードが違います。"), status_code=401)
    resp = RedirectResponse("/", status_code=303)
    resp.set_cookie(
        COOKIE_NAME,
        session_token(settings["session_secret"]),
        httponly=True,
        max_age=60 * 60 * 24 * 30,
        samesite="lax",
    )
    return resp


@app.post("/logout")
def logout() -> Response:
    resp = RedirectResponse("/", status_code=303)
    resp.delete_cookie(COOKIE_NAME)
    return resp


# --- 状態 -------------------------------------------------------------------


@app.get("/status")
def status(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    return JSONResponse(
        {
            "ok": True,
            "llm": llm_reachable(),
            "stackchan": stackchan_reachable(settings),
            "android_ips": local_ip_candidates(),
            "stackchan_host": settings.get("stackchan_host", ""),
            "stackchan_port": settings.get("stackchan_port", 80),
            "current": CURRENT_WAV.exists(),
            "history_count": len(list_history(max_items=100000)),
            "settings_saved": SETTINGS_PATH.exists(),
            "settings": public_settings(settings),
        }
    )


def public_settings(settings: Dict[str, Any]) -> Dict[str, Any]:
    """パスワード等の秘匿項目を除いた設定を返す。"""
    keys = [
        "stackchan_host", "stackchan_port", "voice_lang", "tts_voice", "pitch", "volume",
        "speed", "answer_mode", "kanji_to_kana", "auto_speak", "max_history",
        "system_prompt",
    ]
    return {k: settings.get(k) for k in keys}


# --- 設定保存 ---------------------------------------------------------------


@app.post("/settings")
async def update_settings(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    form = await request.form()

    if form.get("stackchan_host") is not None:
        settings["stackchan_host"] = str(form.get("stackchan_host")).strip()
    if form.get("stackchan_port") is not None:
        settings["stackchan_port"] = clamp(form.get("stackchan_port"), 1, 65535, 80)
    if form.get("voice_lang") is not None:
        settings["voice_lang"] = norm_lang(form.get("voice_lang"))
    if form.get("tts_voice") is not None:
        settings["tts_voice"] = norm_tts_voice(form.get("tts_voice"))
    if form.get("pitch") is not None:
        settings["pitch"] = clamp(form.get("pitch"), 0, 99, 80)
    if form.get("volume") is not None:
        settings["volume"] = clamp(form.get("volume"), 0, 200, 200)
    if form.get("speed") is not None:
        settings["speed"] = clamp(form.get("speed"), 80, 260, 150)
    if form.get("answer_mode") is not None:
        settings["answer_mode"] = norm_mode(form.get("answer_mode"))
    if form.get("kanji_to_kana") is not None:
        settings["kanji_to_kana"] = str(form.get("kanji_to_kana")).lower() in ("1", "true", "on", "yes")
    if form.get("auto_speak") is not None:
        settings["auto_speak"] = str(form.get("auto_speak")).lower() in ("1", "true", "on", "yes")
    if form.get("max_history") is not None:
        settings["max_history"] = clamp(form.get("max_history"), 1, 100000, 50)
    if form.get("system_prompt") is not None:
        settings["system_prompt"] = str(form.get("system_prompt"))

    # パスワード変更（任意）
    new_pw = form.get("new_password")
    if new_pw:
        salt = secrets.token_hex(16)
        settings["password_salt"] = salt
        settings["password_hash"] = hash_password(str(new_pw), salt)

    try:
        save_settings(settings)
    except Exception as exc:
        return JSONResponse({"ok": False, "error": f"save failed: {exc}"}, status_code=500)
    return JSONResponse({"ok": True, "settings": public_settings(settings)})


# --- 質問 -------------------------------------------------------------------


@app.post("/ask")
async def ask(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    form = await request.form()
    question = str(form.get("question", "")).strip()
    if not question:
        return JSONResponse({"ok": False, "error": "質問が空です。"}, status_code=400)

    lang = norm_lang(form.get("voice_lang", settings.get("voice_lang", "ja")))
    mode = norm_mode(form.get("answer_mode", settings.get("answer_mode", "short")))

    try:
        answer = call_llm(question, settings, lang, mode)
    except Exception as exc:
        return JSONResponse(
            {"ok": False, "error": f"LLM生成に失敗しました: {exc}"}, status_code=502
        )

    kanji_to_kana = form.get("kanji_to_kana")
    if kanji_to_kana is None:
        kanji_to_kana = bool(settings.get("kanji_to_kana", True))
    else:
        kanji_to_kana = str(kanji_to_kana).lower() in ("1", "true", "on", "yes")

    speech_lang = speech_lang_for(answer, lang)
    speech_text = make_speech_text(answer, speech_lang, bool(kanji_to_kana))
    pitch = clamp(form.get("pitch", settings.get("pitch")), 0, 99, 80)
    volume = clamp(form.get("volume", settings.get("volume")), 0, 200, 200)
    speed = clamp(form.get("speed", settings.get("speed")), 80, 260, 150)
    tts_voice = norm_tts_voice(form.get("tts_voice", settings.get("tts_voice", "default")))
    item = build_item(
        question,
        answer,
        speech_text,
        lang,
        pitch,
        volume,
        speed,
        mode,
        tts_voice=tts_voice,
        speech_lang=speech_lang,
    )

    tts_ok = True
    tts_error = ""
    try:
        synth_wav(speech_text, speech_lang, pitch, volume, speed, history_wav_path(item["id"]), tts_voice)
    except Exception as exc:
        tts_ok = False
        tts_error = str(exc)

    save_history_item(item)
    if tts_ok:
        set_current(item)
        prune_history(clamp(settings.get("max_history"), 1, 100000, 50))

    speak_result: Optional[Dict[str, Any]] = None
    if tts_ok and settings.get("auto_speak", True):
        speak_result = call_stackchan_speak(settings, "__CURRENT__")

    return JSONResponse(
        {
            "ok": True,
            "item": item,
            "tts_ok": tts_ok,
            "tts_error": tts_error,
            "speak": speak_result,
        }
    )


# --- 音声だけ再生成 ----------------------------------------------------------


@app.post("/voice/rebuild")
async def voice_rebuild(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    form = await request.form()
    item_id = form.get("id")

    if item_id:
        item = load_history_item(str(item_id))
    else:
        item = load_current()
    if not item:
        return JSONResponse({"ok": False, "error": "対象がありません。"}, status_code=404)

    overrides = {
        "voice_lang": form.get("voice_lang"),
        "tts_voice": form.get("tts_voice"),
        "pitch": form.get("pitch"),
        "volume": form.get("volume"),
        "speed": form.get("speed"),
    }
    if form.get("kanji_to_kana") is not None:
        overrides["kanji_to_kana"] = str(form.get("kanji_to_kana")).lower() in ("1", "true", "on", "yes")
    overrides = {k: v for k, v in overrides.items() if v is not None}

    try:
        item = revoice_item(item, settings, overrides)
    except Exception as exc:
        return JSONResponse({"ok": False, "error": f"音声生成に失敗: {exc}"}, status_code=500)

    # current を再生成した場合は current.wav も更新する
    current = load_current()
    if current and current.get("id") == item["id"]:
        set_current(item)

    return JSONResponse({"ok": True, "item": item})


# --- WAV 配信 ---------------------------------------------------------------


@app.get("/current.wav")
def current_wav() -> Response:
    if CURRENT_WAV.exists():
        return FileResponse(CURRENT_WAV, media_type="audio/wav")
    return FileResponse(ensure_default_wav(), media_type="audio/wav")


@app.get("/history/{item_id}.wav")
def history_wav(item_id: str) -> Response:
    path = history_wav_path(item_id)
    if path.exists():
        return FileResponse(path, media_type="audio/wav")
    return JSONResponse({"ok": False, "error": "not found"}, status_code=404)


# --- StackChan 互換 /synthesis ---------------------------------------------


@app.post("/synthesis")
async def synthesis(request: Request) -> Response:
    """StackChan(simple_wav) が叩くエンドポイント。常に audio/wav を返す。

    - 本文 "__REASK_LAST__": 最後の質問を再LLM処理して新WAVを返す
    - 本文 "__SAY__<文章>": 文章をそのまま読み上げた WAV を返す（プリンタ実況用）
    - 本文 "__CURRENT__" / その他: current.wav を返す
    - current.wav が無ければデフォルトWAVを返す
    """
    body = (await request.body()).decode("utf-8", errors="ignore").strip()
    settings = load_settings()

    if body.startswith(SAY_PREFIX):
        wav = synth_say_wav(body[len(SAY_PREFIX):], settings)
        if wav is not None:
            return FileResponse(wav, media_type="audio/wav")
        return FileResponse(ensure_default_wav(), media_type="audio/wav")

    if body == "__REASK_LAST__":
        current = load_current()
        last_q = current.get("question") if current else None
        if last_q:
            lang = norm_lang(current.get("voice_lang", settings.get("voice_lang", "ja")))
            mode = norm_mode(current.get("answer_mode", settings.get("answer_mode", "short")))
            try:
                process_question(last_q, settings, lang, mode)
            except Exception:
                pass  # 失敗時は既存 current.wav にフォールバック

    if CURRENT_WAV.exists():
        return FileResponse(CURRENT_WAV, media_type="audio/wav")
    return FileResponse(ensure_default_wav(), media_type="audio/wav")


# --- StackChan へ発話指示 ---------------------------------------------------


@app.post("/stackchan/speak")
def stackchan_speak(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    result = call_stackchan_speak(settings, "__CURRENT__")
    return JSONResponse(result)


# --- 履歴 -------------------------------------------------------------------


@app.get("/history")
def history(request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    return JSONResponse({"ok": True, "items": list_history(max_items=200)})


@app.post("/history/{item_id}/speak")
def history_speak(item_id: str, request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    item = load_history_item(item_id)
    if not item:
        return JSONResponse({"ok": False, "error": "not found"}, status_code=404)
    set_current(item)
    result = call_stackchan_speak(settings, "__CURRENT__")
    return JSONResponse({"ok": True, "speak": result, "item": item})


@app.post("/history/{item_id}/revoice")
async def history_revoice(item_id: str, request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    item = load_history_item(item_id)
    if not item:
        return JSONResponse({"ok": False, "error": "not found"}, status_code=404)
    form = await request.form()
    overrides = {
        "voice_lang": form.get("voice_lang"),
        "tts_voice": form.get("tts_voice"),
        "pitch": form.get("pitch"),
        "volume": form.get("volume"),
        "speed": form.get("speed"),
    }
    if form.get("kanji_to_kana") is not None:
        overrides["kanji_to_kana"] = str(form.get("kanji_to_kana")).lower() in ("1", "true", "on", "yes")
    overrides = {k: v for k, v in overrides.items() if v is not None}
    set_as_current = str(form.get("set_current", "")).lower() in ("1", "true", "on", "yes")

    try:
        item = revoice_item(item, settings, overrides)
    except Exception as exc:
        return JSONResponse({"ok": False, "error": f"音声生成に失敗: {exc}"}, status_code=500)

    if set_as_current:
        set_current(item)
    else:
        current = load_current()
        if current and current.get("id") == item["id"]:
            set_current(item)
    return JSONResponse({"ok": True, "item": item})


@app.post("/history/{item_id}/delete")
def history_delete(item_id: str, request: Request) -> JSONResponse:
    settings = require_login(request)
    if settings is None:
        return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)
    history_json_path(item_id).unlink(missing_ok=True)
    history_wav_path(item_id).unlink(missing_ok=True)
    return JSONResponse({"ok": True})


# ----------------------------------------------------------------------------
# HTML（ログイン画面・メインUI）。UI ロジックは ui.py から読み込む。
# ----------------------------------------------------------------------------

from ui import LOGIN_HTML, MAIN_HTML  # noqa: E402


if __name__ == "__main__":
    import uvicorn

    ensure_dirs()
    load_settings()
    uvicorn.run(app, host="0.0.0.0", port=50021)
