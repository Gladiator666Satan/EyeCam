#!/usr/bin/env bash
# ==============================================================================
# Скрипт ініціалізації та конфігурації ядра Linux для віртуальної камери v4l2loopback
# ==============================================================================

set -e

VIDEO_NR=10
DEVICE_NAME="EyeCam Virtual Camera"

echo "======================================================================"
echo " Ініціалізація віртуальної камери EyeCam через v4l2loopback"
echo "======================================================================"

# Перевірка наявності прав суперкористувача (root)
if [ "$EUID" -ne 0 ]; then
  echo "[-] Будь ласка, запустіть цей скрипт з правами sudo:"
  echo "    sudo $0"
  exit 1
fi

# Перевірка наявності утиліт v4l2
if ! command -v v4l2-ctl &> /dev/null; then
    echo "[*] Встановлення необхідних системних пакунків (v4l2loopback, v4l-utils)..."
    if command -v apt-get &> /dev/null; then
        apt-get update && apt-get install -y v4l2loopback-dkms v4l-utils
    elif command -v dnf &> /dev/null; then
        dnf install -y v4l2loopback v4l-utils
    elif command -v pacman &> /dev/null; then
        pacman -Sy --noconfirm v4l2loopback-dkms v4l-utils
    else
        echo "[!] Невідомий пакетний менеджер. Встановіть v4l2loopback вручну."
    fi
fi

# Вивантаження старого екземпляра модуля, якщо він був активний
if lsmod | grep -q "^v4l2loopback"; then
    echo "[*] Вивантаження попередньої сесії v4l2loopback..."
    rmmod v4l2loopback || true
fi

# Завантаження модуля v4l2loopback з правильними системними прапорцями:
# 1. exclusive_caps=1: КРИТИЧНО ВАЖЛИВО для Google Chrome, Zoom, WebRTC та OBS.
#    Цей параметр приховує прапорець OUTPUT від браузерів та додатків-споживачів,
#    завдяки чому вони сприймають пристрій як справжню фізичну вебкамеру захоплення (CAPTURE).
# 2. video_nr=$VIDEO_NR: фіксує номер ноди (/dev/video10).
# 3. card_label: зрозуміле ім'я пристрою в меню додатків.
echo "[*] Завантаження модуля ядра v4l2loopback..."
modprobe v4l2loopback \
    devices=1 \
    video_nr=${VIDEO_NR} \
    card_label="${DEVICE_NAME}" \
    exclusive_caps=1 \
    max_buffers=4

# Налаштування прав доступу до ноди пристрою
DEV_NODE="/dev/video${VIDEO_NR}"
if [ -e "$DEV_NODE" ]; then
    chmod 666 "$DEV_NODE"
    echo "[+] Віртуальний пристрій створено успішно: $DEV_NODE"
    echo "    Ім'я пристрою: $DEVICE_NAME"
    echo "======================================================================"
    echo "Перевірка характеристик через v4l2-ctl:"
    v4l2-ctl -d "$DEV_NODE" --all || true
else
    echo "[-] Помилка: нода $DEV_NODE не була створена. Перевірте dmesg."
    exit 1
fi
