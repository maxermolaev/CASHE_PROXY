#!/bin/bash

FILE_URL="http://xcal1.vodafone.co.uk/1GB.zip"
PROXY="http://localhost:8080"

echo "Запускаю первый медленный клиент..."
curl -x "$PROXY" "$FILE_URL" \
     --limit-rate 500k \
     -o download1.zip \
     --progress-bar &
PID1=$!

sleep 5   # ждём, пока первый точно начнёт качать

echo "Запускаю второго клиента..."
curl -x "$PROXY" "$FILE_URL" \
     -o download2.zip \
     --progress-bar &
PID2=$!

wait $PID1
wait $PID2

echo "Готово!"
