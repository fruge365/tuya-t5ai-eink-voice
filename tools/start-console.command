#!/bin/bash
# 双击运行：启动本地静态服务并在浏览器打开 T5AI 控制台
# 页面必须经 http://127.0.0.1 访问，烧录服务(ws://127.0.0.1:9527)只接受本机 http 来源
cd "$(dirname "$0")" || exit 1
PORT=8899
PAGE="T5AI%E5%BC%80%E5%8F%91%E6%9D%BF%E6%8E%A7%E5%88%B6%E5%8F%B0.html"

if ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  echo "启动本地服务 127.0.0.1:$PORT ..."
  (nohup python3 -m http.server $PORT --bind 127.0.0.1 >/tmp/t5_console_httpd.log 2>&1 &)
  sleep 1
else
  echo "本地服务已在运行 127.0.0.1:$PORT"
fi

if ! pgrep -f "tyutool_cli serve" >/dev/null 2>&1; then
  echo "提示：烧录服务未运行，需要烧录时执行："
  echo "  cd ../TuyaOpen/tools/tyutool && ./tyutool_cli serve --plain"
fi

open "http://127.0.0.1:$PORT/$PAGE"
