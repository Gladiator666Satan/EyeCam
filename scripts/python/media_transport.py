"""
EyeCam Media Plane Transport (UDP Video Streaming Engine)
Реалізація низьколатентної передачі відео через UDP:
- Фрагментація NAL-юнітів (H.264 / HEVC) під розмір MTU (без IP-фрагментації).
- Бінарний заголовок пакету з відстеженням Sequence Number та Frame ID.
- Zero-Retransmission Policy: відсутність повторних запитів окремих втрачених пакетів.
- Детектор втрат: якщо фрагмент кадру втрачено, весь P-кадр негайно відкидається для запобігання артефактам розпакування (Artifact Propagation).
- При втраті I-кадру (Keyframe) ініціюється зворотний виклик для генерації швидкого IDR-кадру (PLI).
"""

import asyncio
import socket
import logging
from typing import Callable, Optional, Dict, List

from eyecam_protocol import (
    MediaPacketHeader,
    MediaFrameFlags,
    MediaCodec,
    MEDIA_HEADER_SIZE,
    get_monotonic_time_us
)

logger = logging.getLogger("MediaTransport")

# Безпечний розмір корисного навантаження (MTU 1500 - 20 IP - 8 UDP - 24 Заголовок = 1448 байтів)
MAX_PAYLOAD_CHUNK_SIZE = 1400


class MediaPacketSender:
    """
    Відправник медіа-потоку (сторона Android пристрою).
    Отримує закодований NALU від апаратного енкодера MediaCodec,
    фрагментує його та транслює через UDP сокет.
    """
    def __init__(self, target_host: str, target_port: int):
        self.target_host = target_host
        self.target_port = target_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Збільшуємо буфер відправки сокета ОС
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        
        self.sequence_num: int = 0
        self.frame_id: int = 0

    def send_frame(self, nalu_bytes: bytes, is_keyframe: bool, pts_us: int, codec: MediaCodec = MediaCodec.H264):
        total_len = len(nalu_bytes)
        # Розрахунок кількості фрагментів
        frag_count = (total_len + MAX_PAYLOAD_CHUNK_SIZE - 1) // MAX_PAYLOAD_CHUNK_SIZE
        if frag_count == 0:
            frag_count = 1

        self.frame_id = (self.frame_id + 1) & 0xFFFFFFFF

        for frag_idx in range(frag_count):
            start_offset = frag_idx * MAX_PAYLOAD_CHUNK_SIZE
            end_offset = min(start_offset + MAX_PAYLOAD_CHUNK_SIZE, total_len)
            chunk = nalu_bytes[start_offset:end_offset]

            flags = MediaFrameFlags.NONE
            if is_keyframe:
                flags |= MediaFrameFlags.KEYFRAME
            if frag_idx == 0:
                flags |= MediaFrameFlags.START_OF_FRAME
            if frag_idx == frag_count - 1:
                flags |= MediaFrameFlags.END_OF_FRAME

            header = MediaPacketHeader(
                flags=flags,
                codec=codec,
                sequence_num=self.sequence_num,
                frame_id=self.frame_id,
                fragment_index=frag_idx,
                fragment_total=frag_count,
                pts_us=pts_us,
                payload_len=len(chunk)
            )

            packet_bytes = header.pack() + chunk
            self.sock.sendto(packet_bytes, (self.target_host, self.target_port))

            self.sequence_num = (self.sequence_num + 1) & 0xFFFF

    def close(self):
        self.sock.close()


class MediaFrameReassembler:
    """
    Збирач фрагментів кадрів на стороні приймача (Desktop).
    Реалізує перевірку цілісності та політику нульової ретрансмісії.
    """
    def __init__(self, on_frame_complete: Callable[[bytes, MediaPacketHeader], None],
                 on_keyframe_needed: Callable[[int, str], None]):
        self.on_frame_complete = on_frame_complete
        self.on_keyframe_needed = on_keyframe_needed
        
        self.expected_seq: Optional[int] = None
        self.current_frame_id: Optional[int] = None
        self.current_fragments: Dict[int, bytes] = {}
        self.current_total_frags: int = 0
        self.current_frame_header: Optional[MediaPacketHeader] = None
        self.is_current_frame_corrupt: bool = False

    def process_packet(self, packet_bytes: bytes):
        res = MediaPacketHeader.unpack(packet_bytes)
        if not res:
            return
        header, chunk = res

        # 1. Перевірка послідовності Sequence Number
        if self.expected_seq is not None:
            if header.sequence_num != self.expected_seq:
                lost_packets = (header.sequence_num - self.expected_seq) & 0xFFFF
                logger.warning(
                    f"[ВТРАТА МЕДІА-ПАКЕТІВ] Пропущено {lost_packets} пакетів! "
                    f"Очікувався seq={self.expected_seq}, отримано seq={header.sequence_num}"
                )
                self.is_current_frame_corrupt = True
                
                # Якщо втрачено пакети ключового кадру - негайно запитуємо новий IDR
                if header.flags & MediaFrameFlags.KEYFRAME:
                    self.on_keyframe_needed(header.frame_id, "keyframe_fragment_lost")

        self.expected_seq = (header.sequence_num + 1) & 0xFFFF

        # 2. Якщо почався новий кадр, але попередній не завершився або був пошкоджений
        if self.current_frame_id is not None and header.frame_id != self.current_frame_id:
            if not self.is_current_frame_corrupt and len(self.current_fragments) == self.current_total_frags:
                self._dispatch_complete_frame()
            else:
                logger.info(
                    f"[ZERO-RETRANSMISSION] Кадр #{self.current_frame_id} відкинуто "
                    f"(зібрано {len(self.current_fragments)}/{self.current_total_frags} фрагментів)"
                )
            # Скидання стану для нового кадру
            self.current_fragments.clear()
            self.is_current_frame_corrupt = False

        self.current_frame_id = header.frame_id
        self.current_total_frags = header.fragment_total
        self.current_frame_header = header
        self.current_fragments[header.fragment_index] = chunk

        # 3. Перевірка завершення кадру
        if (header.flags & MediaFrameFlags.END_OF_FRAME) and not self.is_current_frame_corrupt:
            if len(self.current_fragments) == self.current_total_frags:
                self._dispatch_complete_frame()
                self.current_fragments.clear()

    def _dispatch_complete_frame(self):
        # Збирання фрагментів у правильному порядку
        ordered_chunks = [self.current_fragments[i] for i in range(self.current_total_frags)]
        full_frame = b''.join(ordered_chunks)
        self.on_frame_complete(full_frame, self.current_frame_header)


class MediaPacketReceiver:
    """
    UDP-сервер прийому відеопотоку на стороні Desktop Virtual Webcam.
    """
    def __init__(self, listen_host: str = "0.0.0.0", listen_port: int = 8555,
                 on_frame_ready: Optional[Callable[[bytes, MediaPacketHeader], None]] = None,
                 on_idr_request: Optional[Callable[[int, str], None]] = None):
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Збільшуємо системний буфер сокета для уникнення дропів при сплесках трафіку
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind((self.listen_host, self.listen_port))
        
        self.reassembler = MediaFrameReassembler(
            on_frame_complete=on_frame_ready or self._default_frame_handler,
            on_keyframe_needed=on_idr_request or self._default_idr_handler
        )
        self._is_running = False

    def _default_frame_handler(self, frame_bytes: bytes, header: MediaPacketHeader):
        logger.info(
            f"[ВІДЕО КАДР ОТРИМАНО] Frame #{header.frame_id} | "
            f"Розмір: {len(frame_bytes)} байтів | Keyframe: {bool(header.flags & MediaFrameFlags.KEYFRAME)} | "
            f"PTS: {header.pts_us} us"
        )

    def _default_idr_handler(self, frame_id: int, reason: str):
        logger.warning(f"[IDR CALLBACK] Необхідно надіслати запит IDR через Control Plane! Frame #{frame_id}, Причина: {reason}")

    def start_sync(self):
        """Синхронний цикл обробки для високопродуктивних потоків."""
        self._is_running = True
        logger.info(f"UDP Media Приймач запущено на {self.listen_host}:{self.listen_port}")
        try:
            while self._is_running:
                data, _ = self.sock.recvfrom(2048)
                self.reassembler.process_packet(data)
        except Exception as e:
            logger.error(f"Помилка в MediaPacketReceiver: {e}")
        finally:
            self.sock.close()

    def close(self):
        self._is_running = False
        self.sock.close()
