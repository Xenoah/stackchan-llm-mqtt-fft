#!/data/data/com.termux/files/usr/bin/bash
# StackChan Gateway 停止スクリプト（Android Termux）。
# tmux セッションを終了し、wake-lock を解放する。

for s in stackchan_gateway stackchan_llama; do
  if tmux has-session -t "$s" 2>/dev/null; then
    tmux kill-session -t "$s"
    echo "停止: $s"
  else
    echo "未起動: $s"
  fi
done

command -v termux-wake-unlock >/dev/null 2>&1 && termux-wake-unlock || true
echo "完了。"
