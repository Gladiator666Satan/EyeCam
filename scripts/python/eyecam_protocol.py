"""
EyeCam Ultra-Low-Latency Protocol Specification & Serializer
Модуль містить бінарні структури заголовків для медіа-каналу (Media Plane) 
та контрольного каналу (Control Plane), а також допоміжні класи для серіалізації/десеріалізації.
"""

import struct
import time
import json
from dataclasses import dataclass
from enum import IntEnum, IntFlag
from typing import Optional, Tuple, Dict, Any

# ============================================================================
# 1. КОНСТАНТИ ТА ФЛАГИ МЕДІА-КАНАЛУ (MEDIA PLANE)
# ============================================================================

MEDIA_MAGIC = b'EC'  # EyeCam Media Magic (2 байти)
MEDIA_HEADER_FORMAT = "!2sBBHIHHQH"
MEDIA_HEADER_SIZE = struct.calcsize(MEDIA_HEADER_FORMAT)  # 24 байти

class MediaCodec(IntEnum):
    UNSPECIFIED = 0
    H264 = 1
    HEVC = 2  # H.265
    AV1 = 3

class MediaFrameFlags(IntFlag):
    NONE = 0
    KEYFRAME = 1 << 0       # 0x01: Ключовий кадр (IDR / I-frame / SPS / PPS)
    START_OF_FRAME = 1 << 1 # 0x02: Перший фрагмент кадру
    END_OF_FRAME = 1 << 2   # 0x04: Останній фрагмент кадру

@dataclass
class MediaPacketHeader:
    """
    Бінарний заголовок пакету відео (24 байти):
    -------------------------------------------------------------------------
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |           Magic ('EC')        |     Flags     |   Codec Type  |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |         Sequence Number       |           Frame ID            |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |       Frame ID (cont.)        |         Fragment Index        |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |         Fragment Total        |                               |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
    |                 Presentation Timestamp (PTS)                  |
    |                         (64-bit uint)                         |
    +                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                               |         Payload Length        |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    """
    flags: MediaFrameFlags
    codec: MediaCodec
    sequence_num: int      # uint16: наскрізний лічильник пакетів (для виявлення втрат)
    frame_id: int          # uint32: ідентифікатор кадру
    fragment_index: int    # uint16: номер фрагмента поточного кадру (0-indexed)
    fragment_total: int    # uint16: загальна кількість фрагментів кадру
    pts_us: int            # uint64: мітка часу відтворення (мікросекунди)
    payload_len: int       # uint16: довжина даних після заголовка

    def pack(self) -> bytes:
        return struct.pack(
            MEDIA_HEADER_FORMAT,
            MEDIA_MAGIC,
            int(self.flags),
            int(self.codec),
            self.sequence_num & 0xFFFF,
            self.frame_id & 0xFFFFFFFF,
            self.fragment_index & 0xFFFF,
            self.fragment_total & 0xFFFF,
            self.pts_us,
            self.payload_len & 0xFFFF
        )

    @classmethod
    def unpack(cls, buffer: bytes) -> Optional[Tuple['MediaPacketHeader', bytes]]:
        if len(buffer) < MEDIA_HEADER_SIZE:
            return None
        
        magic, flags, codec, seq, frame_id, frag_idx, frag_tot, pts, plen = struct.unpack(
            MEDIA_HEADER_FORMAT, buffer[:MEDIA_HEADER_SIZE]
        )
        
        if magic != MEDIA_MAGIC:
            raise ValueError(f"Невірний магічний байт: {magic}")
            
        header = cls(
            flags=MediaFrameFlags(flags),
            codec=MediaCodec(codec),
            sequence_num=seq,
            frame_id=frame_id,
            fragment_index=frag_idx,
            fragment_total=frag_tot,
            pts_us=pts,
            payload_len=plen
        )
        payload = buffer[MEDIA_HEADER_SIZE:MEDIA_HEADER_SIZE + plen]
        return header, payload

# ============================================================================
# 2. КОНСТАНТИ ТА ЗАГОЛОВОК КОНТРОЛЬНОГО КАНАЛУ (CONTROL PLANE)
# ============================================================================

CONTROL_MAGIC = b'CP'  # Control Protocol Magic (2 байти)
CONTROL_HEADER_FORMAT = "!2sHI"  # Magic (2), Type (uint16), Length (uint32)
CONTROL_HEADER_SIZE = struct.calcsize(CONTROL_HEADER_FORMAT)  # 8 байтів

class ControlMessageType(IntEnum):
    JSON_COMMAND = 1         # Текстова JSON-команда керування / статус
    PROTOBUF_COMMAND = 2     # Бінарне Protobuf-повідомлення
    FRAME_PING = 3           # Запит синхронізації часу та RTT
    FRAME_PONG = 4           # Відповідь синхронізації часу та RTT
    KEYFRAME_REQUEST = 5     # Терміновий запит ключового кадру (IDR/PLI)
    KEEP_ALIVE = 6           # Серцебиття (Heartbeat)

@dataclass
class ControlFrame:
    """
    Кадр керування над потоковим TCP/TLS транспортом:
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |           Magic ('CP')        |          Message Type         |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                     Payload Length (uint32)                   |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                        Payload Bytes...                       |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    """
    msg_type: ControlMessageType
    payload: bytes

    def pack(self) -> bytes:
        header = struct.pack(
            CONTROL_HEADER_FORMAT,
            CONTROL_MAGIC,
            int(self.msg_type),
            len(self.payload)
        )
        return header + self.payload

    @classmethod
    def from_json(cls, data: Dict[str, Any], msg_type: ControlMessageType = ControlMessageType.JSON_COMMAND) -> 'ControlFrame':
        payload = json.dumps(data, separators=(',', ':')).encode('utf-8')
        return cls(msg_type=msg_type, payload=payload)

    def to_json(self) -> Dict[str, Any]:
        return json.loads(self.payload.decode('utf-8'))

# ============================================================================
# 3. ДОПОМІЖНІ УТИЛІТИ ЧАСУ ТА RTT
# ============================================================================

def get_monotonic_time_us() -> int:
    """Повертає монотонний час у мікросекундах (для RTT і PTS без стрибків системного годинника)."""
    return int(time.monotonic() * 1_000_000)

def calculate_ntp_metrics(t1: int, t2: int, t3: int, t4: int) -> Tuple[float, float]:
    """
    Розрахунок Round Trip Time (RTT) та зсуву годинника (Clock Offset) за алгоритмом NTP:
    t1: Час відправки клієнтом (Client TX)
    t2: Час отримання сервером (Server RX)
    t3: Час відправки відповіді сервером (Server TX)
    t4: Час отримання відповіді клієнтом (Client RX)

    Повертає: (rtt_ms, clock_offset_ms)
    """
    # Загальний час туди й назад за мінусом часу обробки на сервері
    rtt = (t4 - t1) - (t3 - t2)
    # Зсув годинника клієнта відносно сервера
    offset = ((t2 - t1) + (t3 - t4)) / 2.0
    return rtt / 1000.0, offset / 1000.0
