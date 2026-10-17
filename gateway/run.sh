#!/data/data/com.termux/files/usr/bin/bash
# StackChan Gateway 起動スクリプト（Android Termux）。
# llama-server と Gateway(FastAPI) を tmux セッションで起動し、
# termux-wake-lock でスリープを防ぐ。ログは ~/stackchan_gateway/logs/ に出力。
set -e

GW_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$HOME/stackchan_gateway"
LOG_DIR="$DATA_DIR/logs"
mkdir -p "$LOG_DIR"

# モデル・llama-server のパス（必要に応じて変更）
LLAMA_BIN="${LLAMA_BIN:-$HOME/llama.cpp/build/bin/llama-server}"
MODEL="${MODEL:-$HOME/models/model.gguf}"
LLAMA_HOST="127.0.0.1"
LLAMA_PORT="8080"

# スリープ防止
command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock || true

# --- llama-server ---
if tmux has-session -t stackchan_llama 2>/dev/null; then
  echo "llama-server は既に起動中です (tmux: stackchan_llama)"
else
  if [ ! -x "$LLAMA_BIN" ]; then
    echo "警告: llama-server が見つかりません: $LLAMA_BIN"
    echo "      LLAMA_BIN / MODEL 環境変数で指定してください。"
  else
    echo "llama-server を起動します…"
    tmux new-session -d -s stackchan_llama \
      "'$LLAMA_BIN' --host $LLAMA_HOST --port $LLAMA_PORT --alias local -m '$MODEL' \
       2>&1 | tee -a '$LOG_DIR/llama.log'"
  fi
fi

# --- Gateway ---
if tmux has-session -t stackchan_gateway 2>/dev/null; then
  echo "Gateway は既に起動中です (tmux: stackchan_gateway)"
else
  echo "Gateway を起動します… (http://0.0.0.0:50021)"
  tmux new-session -d -s stackchan_gateway \
    "cd '$GW_DIR' && python -m uvicorn app:app --host 0.0.0.0 --port 50021 \
     2>&1 | tee -a '$LOG_DIR/gateway.log'"
fi

echo
echo "起動しました。"
echo "  Gateway UI : http://<このスマホのLAN内IP>:50021"
echo "  ログ       : $LOG_DIR"
echo "  停止       : $GW_DIR/stop.sh"
echo "  セッション : tmux attach -t stackchan_gateway / stackchan_llama"
