package com.eyecam.network

import android.util.Log
import com.eyecam.model.NalType
import com.eyecam.model.NalUnit
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * High-performance low-latency UDP media packetizer and transmitter.
 * Conforms to the NexusCam / EyeCam 24-byte binary framing contract (Big-Endian network order):
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Magic ('EC')        |     Flags     |   Codec Type  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         Sequence Number       |           Frame ID            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       Frame ID (cont.)        |         Fragment Index        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         Fragment Total        |                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
 * |                 Presentation Timestamp (PTS in us)            |
 * |                         (64-bit uint)                         |
 * +                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                               |         Payload Length        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
class UdpMediaSender(
    private val targetHost: String = "127.0.0.1",
    private val targetPort: Int = 8555
) {
    companion object {
        private const val TAG = "UdpMediaSender"

        // 24-byte binary header constants
        const val MEDIA_HEADER_SIZE = 24
        const val MAGIC_BYTE_0 = 'E'.code.toByte() // 0x45
        const val MAGIC_BYTE_1 = 'C'.code.toByte() // 0x43

        // Max payload chunk to avoid IP fragmentation (MTU 1500 - 20 IP - 8 UDP - 24 Header = 1448)
        const val MAX_PAYLOAD_CHUNK_SIZE = 1400

        // Codec IDs
        const val CODEC_UNSPECIFIED: Byte = 0
        const val CODEC_H264: Byte = 1
        const val CODEC_HEVC: Byte = 2
        const val CODEC_AV1: Byte = 3

        // Flags
        const val FLAG_KEYFRAME: Byte = 0x01       // 1 shl 0
        const val FLAG_START_OF_FRAME: Byte = 0x02 // 1 shl 1
        const val FLAG_END_OF_FRAME: Byte = 0x04   // 1 shl 2
    }

    private var socket: DatagramSocket? = null
    private var targetAddress: InetAddress? = null

    private var sequenceNumber: Short = 0
    private var frameId: Int = 0

    @Synchronized
    fun start() {
        if (socket != null && !socket!!.isClosed) return
        try {
            socket = DatagramSocket().apply {
                sendBufferSize = 2 * 1024 * 1024 // 2MB kernel send buffer
            }
            targetAddress = InetAddress.getByName(targetHost)
            Log.i(TAG, "UdpMediaSender started. Destination: $targetHost:$targetPort")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to initialize DatagramSocket", e)
        }
    }

    @Synchronized
    fun stop() {
        try {
            socket?.close()
        } catch (_: Exception) {}
        socket = null
        targetAddress = null
        Log.i(TAG, "UdpMediaSender stopped.")
    }

    /**
     * Fragments and sends an encoded NALUnit over UDP with 24-byte binary framing.
     */
    fun sendNalUnit(nalUnit: NalUnit) {
        val currentSocket = socket ?: return
        val currentAddress = targetAddress ?: return

        val rawBytes = nalUnit.data
        val totalBytes = rawBytes.size
        if (totalBytes == 0) return

        frameId++
        val ptsUs = nalUnit.presentationTimestampUs
        val isKey = nalUnit.isKeyFrame || nalUnit.type == NalType.IDR_FRAME || nalUnit.type == NalType.SPS

        val codecByte = if (nalUnit.type == NalType.VPS) CODEC_HEVC else CODEC_H264

        val fragmentTotal = ((totalBytes + MAX_PAYLOAD_CHUNK_SIZE - 1) / MAX_PAYLOAD_CHUNK_SIZE).coerceAtLeast(1)

        val packetBuffer = ByteBuffer.allocate(MEDIA_HEADER_SIZE + MAX_PAYLOAD_CHUNK_SIZE)
        packetBuffer.order(ByteOrder.BIG_ENDIAN)

        for (fragIdx in 0 until fragmentTotal) {
            val offset = fragIdx * MAX_PAYLOAD_CHUNK_SIZE
            val chunkSize = (totalBytes - offset).coerceAtMost(MAX_PAYLOAD_CHUNK_SIZE)

            var flags: Byte = 0
            if (isKey) flags = (flags.toInt() or FLAG_KEYFRAME.toInt()).toByte()
            if (fragIdx == 0) flags = (flags.toInt() or FLAG_START_OF_FRAME.toInt()).toByte()
            if (fragIdx == fragmentTotal - 1) flags = (flags.toInt() or FLAG_END_OF_FRAME.toInt()).toByte()

            packetBuffer.clear()

            // 1. Magic (2 bytes: 'E', 'C')
            packetBuffer.put(MAGIC_BYTE_0)
            packetBuffer.put(MAGIC_BYTE_1)

            // 2. Flags (1 byte) & Codec (1 byte)
            packetBuffer.put(flags)
            packetBuffer.put(codecByte)

            // 3. Sequence Number (2 bytes uint16, Big-Endian)
            packetBuffer.putShort(sequenceNumber)
            sequenceNumber = (sequenceNumber + 1).toShort()

            // 4. Frame ID (4 bytes uint32, Big-Endian)
            packetBuffer.putInt(frameId)

            // 5. Fragment Index (2 bytes uint16) & Total (2 bytes uint16)
            packetBuffer.putShort(fragIdx.toShort())
            packetBuffer.putShort(fragmentTotal.toShort())

            // 6. Presentation Timestamp (8 bytes uint64, Big-Endian)
            packetBuffer.putLong(ptsUs)

            // 7. Payload Length (2 bytes uint16, Big-Endian)
            packetBuffer.putShort(chunkSize.toShort())

            // 8. Payload slice
            packetBuffer.put(rawBytes, offset, chunkSize)

            val datagram = DatagramPacket(
                packetBuffer.array(),
                0,
                MEDIA_HEADER_SIZE + chunkSize,
                currentAddress,
                targetPort
            )

            try {
                currentSocket.send(datagram)
            } catch (e: IOException) {
                Log.w(TAG, "Error sending UDP packet seq=$sequenceNumber", e)
                break
            }
        }
    }
}
