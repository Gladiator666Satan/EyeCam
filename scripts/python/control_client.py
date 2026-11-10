"""
EyeCam Control Client Reference Implementation (Desktop Virtual Webcam Controller)
Реалізація клієнта керування (Desktop side для Windows / Linux):
- Підключення до камери через Wi-Fi IP або через USB (localhost через adb forward).
- Автоматичний фоновий Keep-Alive та NTP-подібний Ping/RTT моніторинг.
- Обчислення Round-Trip Time (RTT), зсуву годинника та оцінки джиттеру.
- Відправка команд керування сенсором (ISO, Focus, Shutter, Sensor Switch).
- Запит миттєвого ключового кадру (IDR Request) у разі виявлення втрати кадрів.
"""

import asyncio
import socket
import logging
import json
from typing import Optional, Dict, Any

from eyecam_protocol import (
    ControlFrame,
    ControlMessageType,
    CONTROL_HEADER_SIZE,
    get_monotonic_time_us,
    calculate_ntp_metrics
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [Desktop-Control-Client] %(message)s"
)
logger = logging.getLogger("ControlClient")


class EyeCamControlClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 8554):
        self.host = host
        self.port = port
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None
        self._is_running = False

        # Метрики затримки
        self.latest_rtt_ms: float = 0.0
        self.latest_clock_offset_ms: float = 0.0
        self.smoothed_rtt_ms: float = 0.0
        self.jitter_ms: float = 0.0

        # Черга очікування відповідей (ACK)
        self._pending_acks: Dict[str, asyncio.Future] = {}

    async def connect(self):
        """Встановлення TCP-з'єднання з вимкненим Nagle (TCP_NODELAY)."""
        logger.info(f"Підключення до EyeCam пристрою на {self.host}:{self.port}...")
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)

        sock = self.writer.get_extra_info('socket')
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        self._is_running = True
        logger.info("Успішно підключено до EyeCam Control Server.")

        # Запуск фонових задач
        asyncio.create_task(self._reader_loop())
        asyncio.create_task(self._ping_rtt_loop(interval_sec=0.5))

    async def _send_frame(self, frame: ControlFrame):
        """Відправка кадру через TCP потік."""
        if not self.writer or self.writer.is_closing():
            raise ConnectionError("З'єднання з сервером розірвано")
        self.writer.write(frame.pack())
        await self.writer.drain()

    async def _reader_loop(self):
        """Фонове зчитування повідомлень від смартфона."""
        try:
            while self._is_running and self.reader and not self.reader.at_eof():
                header_data = await self.reader.readexactly(CONTROL_HEADER_SIZE)
                magic = header_data[:2]
                if magic != b'CP':
                    logger.error(f"Невірний магічний байт від сервера: {magic}")
                    break

                msg_type_raw = int.from_bytes(header_data[2:4], 'big')
                payload_len = int.from_bytes(header_data[4:8], 'big')
                payload = await self.reader.readexactly(payload_len)

                rx_time_us = get_monotonic_time_us()
                await self._handle_incoming_frame(ControlMessageType(msg_type_raw), payload, rx_time_us)

        except asyncio.IncompleteReadError:
            logger.warning("Сервер закрив з'єднання.")
        except Exception as e:
            logger.error(f"Помилка в циклі зчитування: {e}")
        finally:
            self._is_running = False

    async def _handle_incoming_frame(self, msg_type: ControlMessageType, payload: bytes, rx_time_us: int):
        """Обробка вхідних відповідей: Pong, ACK, Телеметрія."""
        if msg_type == ControlMessageType.FRAME_PONG:
            data = json.loads(payload.decode('utf-8'))
            t1 = data["t1_us"]
            t2 = data["t2_us"]
            t3 = data["t3_us"]
            t4 = rx_time_us

            rtt_ms, offset_ms = calculate_ntp_metrics(t1, t2, t3, t4)
            self._update_rtt_stats(rtt_ms, offset_ms)

        elif msg_type == ControlMessageType.JSON_COMMAND:
            data = json.loads(payload.decode('utf-8'))
            msg_type_name = data.get("type")

            if msg_type_name == "ack":
                action = data.get("action")
                logger.info(f"[ACK] Підтверджено команду '{action}', успіх: {data.get('success')}")
                if action in self._pending_acks:
                    fut = self._pending_acks.pop(action)
                    if not fut.done():
                        fut.set_result(data)

            elif msg_type_name == "device_state":
                state = data.get("state", {})
                logger.debug(
                    f"[СТАТУС] Модуль: {state.get('active_module')} | "
                    f"FPS: {state.get('measured_fps')} | Батарея: {state.get('battery_level')}% | "
                    f"Температура: {state.get('thermal_temp_c')}°C"
                )

    def _update_rtt_stats(self, rtt_ms: float, offset_ms: float):
        """Оновлення ковзного середнього RTT та джиттеру."""
        self.latest_rtt_ms = rtt_ms
        self.latest_clock_offset_ms = offset_ms

        if self.smoothed_rtt_ms == 0.0:
            self.smoothed_rtt_ms = rtt_ms
            self.jitter_ms = 0.0
        else:
            diff = abs(rtt_ms - self.smoothed_rtt_ms)
            self.jitter_ms += (diff - self.jitter_ms) * 0.125
            self.smoothed_rtt_ms += (rtt_ms - self.smoothed_rtt_ms) * 0.125

        logger.info(
            f"[RTT Метрика] Поточний RTT: {rtt_ms:.2f} ms | "
            f"Згладжений: {self.smoothed_rtt_ms:.2f} ms | "
            f"Джиттер: {self.jitter_ms:.2f} ms | Зсув годинника: {offset_ms:.2f} ms"
        )

    async def _ping_rtt_loop(self, interval_sec: float = 0.5):
        """Фоновий цикл вимірювання затримки мережі (Ping/Pong)."""
        while self._is_running:
            t1 = get_monotonic_time_us()
            ping_payload = {"t1_us": t1}
            frame = ControlFrame.from_json(ping_payload, ControlMessageType.FRAME_PING)
            try:
                await self._send_frame(frame)
            except Exception as e:
                logger.error(f"Не вдалося надіслати FramePing: {e}")
                break
            await asyncio.sleep(interval_sec)

    # =========================================================================
    # Публічні методи керування камерою з Desktop-додатку
    # =========================================================================

    async def set_camera_module(self, module_name: str):
        """Перемикання модуля камери: BACK_MAIN, BACK_ULTRAWIDE, BACK_TELEPHOTO, FRONT."""
        logger.info(f"Перемикання сенсора на: {module_name}")
        frame = ControlFrame.from_json({
            "action": "SetCameraModule",
            "module": module_name
        })
        await self._send_frame(frame)

    async def set_iso(self, auto: bool, iso_value: int = 100):
        """Встановлення чутливості ISO."""
        logger.info(f"Налаштування ISO: Auto={auto}, Значення={iso_value}")
        frame = ControlFrame.from_json({
            "action": "SetISO",
            "auto": auto,
            "iso": iso_value
        })
        await self._send_frame(frame)

    async def set_focus(self, mode: str = "CONTINUOUS_VIDEO", distance_diopters: float = 0.0):
        """Налаштування фокусування: CONTINUOUS_VIDEO, AUTO, MANUAL (з діоптріями 0.0 - 10.0)."""
        logger.info(f"Налаштування фокусу: Режим={mode}, Відстань={distance_diopters} дптр")
        frame = ControlFrame.from_json({
            "action": "SetFocus",
            "mode": mode,
            "distance_diopters": distance_diopters
        })
        await self._send_frame(frame)

    async def set_shutter(self, auto: bool, exposure_ns: int = 20_000_000):
        """Налаштування витримки (часу експозиції в наносекундах)."""
        logger.info(f"Налаштування витримки: Auto={auto}, Час={exposure_ns} нс")
        frame = ControlFrame.from_json({
            "action": "SetShutter",
            "auto": auto,
            "exposure_ns": exposure_ns
        })
        await self._send_frame(frame)

    async def set_white_balance(self, mode: str = "AUTO", kelvin: int = 5500):
        """Налаштування балансу білого."""
        logger.info(f"Налаштування балансу білого: Режим={mode}, Колірна темп={kelvin} K")
        frame = ControlFrame.from_json({
            "action": "SetWhiteBalance",
            "mode": mode,
            "kelvin": kelvin
        })
        await self._send_frame(frame)

    async def request_keyframe(self, reason: str = "packet_loss", last_frame_id: int = 0):
        """Терміновий запит ключового кадру (IDR) при втраті пакетів у Media Plane."""
        logger.warning(f"Відправка запиту IDR кадру! Причина: {reason}, останній Frame ID: {last_frame_id}")
        frame = ControlFrame.from_json({
            "reason": reason,
            "last_received_frame_id": last_frame_id
        }, ControlMessageType.KEYFRAME_REQUEST)
        await self._send_frame(frame)

    async def close(self):
        self._is_running = False
        if self.writer:
            self.writer.close()
            await self.writer.wait_closed()


async def demo_client_workflow():
    """Демонстраційний сценарій тестування клієнта."""
    client = EyeCamControlClient(host="127.0.0.1", port=8554)
    await client.connect()

    # Даємо час на виконання перших 2-3 пінґів для вимірювання RTT
    await asyncio.sleep(1.2)

    # Виконання команд керування
    await client.set_camera_module("BACK_MAIN")
    await asyncio.sleep(0.5)

    await client.set_iso(auto=False, iso_value=400)
    await asyncio.sleep(0.5)

    await client.set_focus(mode="MANUAL", distance_diopters=5.0)  # Фокус на відстань 20 см
    await asyncio.sleep(0.5)

    await client.set_white_balance(mode="MANUAL_KELVIN", kelvin=4500)
    await asyncio.sleep(0.5)

    # Імітація виявлення дропу кадру -> запит IDR
    await client.request_keyframe(reason="simulated_packet_loss", last_frame_id=142)

    await asyncio.sleep(2.0)
    await client.close()


if __name__ == "__main__":
    asyncio.run(demo_client_workflow())
