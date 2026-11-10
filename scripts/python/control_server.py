"""
EyeCam Control Server Reference Implementation (Android / Device Side Daemon)
Реалізація керуючого сервера (TCP):
- Підтримка двостороннього обміну повідомленнями.
- TCP_NODELAY для мінімізації затримок (вимкнення алгоритму Нейгла).
- Обробка запитів налаштування камери (ISO, Focus, Shutter, CameraModule).
- Реалізація високоточного NTP-подібного Ping/Pong для вимірювання RTT і джиттеру.
- Відправка періодичної телеметрії (FPS, температура сенсора, заряд акумулятора).
"""

import asyncio
import socket
import logging
import json
from typing import Dict, Any, Optional

from eyecam_protocol import (
    ControlFrame,
    ControlMessageType,
    CONTROL_HEADER_SIZE,
    get_monotonic_time_us
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [Device-Control-Server] %(message)s"
)
logger = logging.getLogger("ControlServer")


class CameraDeviceState:
    """Модель поточного стану апаратних параметрів камери смартфона."""
    def __init__(self):
        self.active_module: str = "BACK_MAIN"
        self.auto_iso: bool = True
        self.current_iso: int = 100
        self.auto_exposure: bool = True
        self.exposure_time_ns: int = 20_000_000  # 1/50 секунди
        self.focus_mode: str = "CONTINUOUS_VIDEO"
        self.focus_distance_diopters: float = 0.0  # 0.0 = нескінченність
        self.wb_mode: str = "AUTO"
        self.wb_temperature_k: int = 5500
        self.battery_level: float = 88.5
        self.thermal_temp_c: float = 34.2
        self.measured_fps: int = 60
        self.measured_bitrate_kbps: int = 15_000

    def to_dict(self) -> Dict[str, Any]:
        return {
            "active_module": self.active_module,
            "auto_iso": self.auto_iso,
            "current_iso": self.current_iso,
            "auto_exposure": self.auto_exposure,
            "exposure_time_ns": self.exposure_time_ns,
            "focus_mode": self.focus_mode,
            "focus_distance_diopters": self.focus_distance_diopters,
            "wb_mode": self.wb_mode,
            "wb_temperature_k": self.wb_temperature_k,
            "battery_level": self.battery_level,
            "thermal_temp_c": self.thermal_temp_c,
            "measured_fps": self.measured_fps,
            "measured_bitrate_kbps": self.measured_bitrate_kbps
        }


class EyeCamControlServer:
    def __init__(self, host: str = "0.0.0.0", port: int = 8554):
        self.host = host
        self.port = port
        self.state = CameraDeviceState()
        self._server: Optional[asyncio.Server] = None
        self._active_writer: Optional[asyncio.StreamWriter] = None

    async def start(self):
        """Запуск TCP-сервера керування."""
        self._server = await asyncio.start_server(
            self._handle_client,
            self.host,
            self.port,
            reuse_address=True
        )
        logger.info(f"Сервер контрольного каналу запущено на {self.host}:{self.port}")

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        sock = writer.get_extra_info('socket')
        if sock:
            # Вимикаємо алгоритм Нейгла для миттєвої відправки контрольних пакетів
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        peername = writer.get_extra_info('peername')
        logger.info(f"Клієнт підключився: {peername}")
        self._active_writer = writer

        # Запуск фонового надсилання телеметрії
        telemetry_task = asyncio.create_task(self._telemetry_loop(writer))

        try:
            while not reader.at_eof():
                # Зчитування фіксованого 8-байтного заголовка ControlFrame
                header_data = await reader.readexactly(CONTROL_HEADER_SIZE)
                magic = header_data[:2]
                if magic != b'CP':
                    logger.error(f"Невірний магічний байт: {magic}, розрив з'єднання")
                    break

                # Розпакування: Magic(2), MsgType(2), PayloadLen(4)
                msg_type_raw, payload_len = int.from_bytes(header_data[2:4], 'big'), int.from_bytes(header_data[4:8], 'big')
                payload = await reader.readexactly(payload_len)

                # Час отримання повідомлення (t2 для NTP-подібного пінгу)
                rx_time_us = get_monotonic_time_us()
                await self._process_message(ControlMessageType(msg_type_raw), payload, rx_time_us, writer)

        except asyncio.IncompleteReadError:
            logger.warning(f"Клієнт {peername} розірвав з'єднання.")
        except Exception as e:
            logger.error(f"Помилка обробки клієнта {peername}: {e}", exc_info=True)
        finally:
            telemetry_task.cancel()
            self._active_writer = None
            writer.close()
            await writer.wait_closed()
            logger.info(f"З'єднання з {peername} закрито.")

    async def _process_message(
        self,
        msg_type: ControlMessageType,
        payload: bytes,
        rx_time_us: int,
        writer: asyncio.StreamWriter
    ):
        """Маршрутизація та виконання отриманих повідомлень."""
        if msg_type == ControlMessageType.FRAME_PING:
            # Обробка FramePing -> надсилання FramePong
            ping_data = json.loads(payload.decode('utf-8'))
            t1 = ping_data.get("t1_us", 0)
            t2 = rx_time_us
            t3 = get_monotonic_time_us()

            pong_payload = {
                "t1_us": t1,
                "t2_us": t2,
                "t3_us": t3
            }
            frame = ControlFrame.from_json(pong_payload, ControlMessageType.FRAME_PONG)
            writer.write(frame.pack())
            await writer.drain()

        elif msg_type == ControlMessageType.KEYFRAME_REQUEST:
            # Запит миттєвого ключового кадру (IDR/PLI) через втрату пакетів у Media Plane
            req = json.loads(payload.decode('utf-8'))
            logger.warning(
                f"[IDR TRIGGER] Отримано запит ключового кадру! Причина: {req.get('reason')}, "
                f"Останній отриманий Frame ID: {req.get('last_received_frame_id')}"
            )
            # Тут ініціюється виклик Camera2 API / MediaCodec:
            # Bundle params = new Bundle();
            # params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            # mediaCodec.setParameters(params);

        elif msg_type == ControlMessageType.JSON_COMMAND:
            cmd = json.loads(payload.decode('utf-8'))
            action = cmd.get("action")
            logger.info(f"Отримано команду керування: {action} -> {cmd}")
            ack_success = True
            err_msg = ""

            try:
                if action == "SetCameraModule":
                    self.state.active_module = cmd.get("module", self.state.active_module)
                elif action == "SetISO":
                    self.state.auto_iso = cmd.get("auto", True)
                    if not self.state.auto_iso:
                        self.state.current_iso = int(cmd.get("iso", 100))
                elif action == "SetFocus":
                    self.state.focus_mode = cmd.get("mode", "AUTO")
                    if self.state.focus_mode == "MANUAL":
                        self.state.focus_distance_diopters = float(cmd.get("distance_diopters", 0.0))
                elif action == "SetShutter":
                    self.state.auto_exposure = cmd.get("auto", True)
                    if not self.state.auto_exposure:
                        self.state.exposure_time_ns = int(cmd.get("exposure_ns", 20_000_000))
                elif action == "SetWhiteBalance":
                    self.state.wb_mode = cmd.get("mode", "AUTO")
                    if self.state.wb_mode == "MANUAL_KELVIN":
                        self.state.wb_temperature_k = int(cmd.get("kelvin", 5500))
                else:
                    ack_success = False
                    err_msg = f"Невідома команда: {action}"
            except Exception as ex:
                ack_success = False
                err_msg = str(ex)

            # Відправка ACK підтвердження
            ack_frame = ControlFrame.from_json({
                "type": "ack",
                "action": action,
                "success": ack_success,
                "error": err_msg
            })
            writer.write(ack_frame.pack())
            await writer.drain()

    async def _telemetry_loop(self, writer: asyncio.StreamWriter):
        """Періодична відправка статусу сенсора та метрик продуктивності (1 раз на секунду)."""
        while True:
            await asyncio.sleep(1.0)
            telemetry_data = {
                "type": "device_state",
                "timestamp_us": get_monotonic_time_us(),
                "state": self.state.to_dict()
            }
            frame = ControlFrame.from_json(telemetry_data, ControlMessageType.JSON_COMMAND)
            try:
                writer.write(frame.pack())
                await writer.drain()
            except Exception:
                break


async def main():
    server = EyeCamControlServer(host="0.0.0.0", port=8554)
    await server.start()
    try:
        await asyncio.Future()  # Нескінченна робота
    except KeyboardInterrupt:
        logger.info("Зупинка сервера за запитом користувача.")


if __name__ == "__main__":
    asyncio.run(main())
