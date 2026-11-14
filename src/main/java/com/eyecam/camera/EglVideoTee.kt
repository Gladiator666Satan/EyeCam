package com.eyecam.camera

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.Matrix
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.io.BufferedReader
import java.io.InputStream
import java.io.InputStreamReader
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.StringTokenizer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Zero-copy OpenGL ES 2.0 video tee with hardware-accelerated 3D LUT (.cube) shader grading.
 *
 * Receives camera frames from a single CameraX [Preview] SurfaceRequest via an OES
 * [SurfaceTexture], and renders each frame independently on a dedicated EGL thread to:
 *  1. The hardware H.264 encoder input [Surface] (landscape/auto-rotated stream with monotonic PTS).
 *  2. The optional UI [TextureView] [Surface] (FIT_CENTER letterboxed, zero stretching).
 *
 * Supports real-time `.cube` 3D LUT color grading packed into a 1024x32 2D texture (32^3 grid).
 */
class EglVideoTee {

    companion object {
        private const val TAG = "EyeCam-EglTee"
        private const val EGL_RECORDABLE_ANDROID = 0x3142
        private const val LUT_GRID_SIZE = 32 // 32x32x32 3D LUT -> 1024x32 2D strip

        private const val VERTEX_SHADER = """
            attribute vec4 aPosition;
            attribute vec4 aTexCoord;
            uniform mat4 uMvpMatrix;
            uniform mat4 uStMatrix;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = uMvpMatrix * aPosition;
                vTexCoord = (uStMatrix * aTexCoord).xy;
            }
        """

        private const val FRAGMENT_SHADER_PASSTHROUGH = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES sTexture;
            void main() {
                gl_FragColor = texture2D(sTexture, vTexCoord);
            }
        """

        private const val FRAGMENT_SHADER_LUT3D = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES sTexture;
            uniform sampler2D sLutTexture;

            vec3 sampleLut3D(vec3 color) {
                vec3 c = clamp(color, 0.0, 1.0);
                float blueCoord = c.b * 31.0;
                float slice0 = floor(blueCoord);
                float slice1 = min(31.0, slice0 + 1.0);
                float fracB = blueCoord - slice0;

                float u0 = (slice0 * 32.0 + c.r * 31.0 + 0.5) / 1024.0;
                float u1 = (slice1 * 32.0 + c.r * 31.0 + 0.5) / 1024.0;
                float v  = (c.g * 31.0 + 0.5) / 32.0;

                vec3 col0 = texture2D(sLutTexture, vec2(u0, v)).rgb;
                vec3 col1 = texture2D(sLutTexture, vec2(u1, v)).rgb;
                return mix(col0, col1, fracB);
            }

            void main() {
                vec4 src = texture2D(sTexture, vTexCoord);
                gl_FragColor = vec4(sampleLut3D(src.rgb), src.a);
            }
        """

        private val QUAD_POSITIONS = floatArrayOf(
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        )

        private val QUAD_TEX_COORDS = floatArrayOf(
            0.0f, 0.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 1.0f
        )

        /**
         * Parses a standard Adobe/Iridas `.cube` 3D (or 1D) LUT stream and bakes it into a
         * 32x32x32 RGBA ByteBuffer (1024 x 32 pixels) ready for `glTexImage2D`.
         */
        fun parseCubeLutToRgba32(inputStream: InputStream): ByteBuffer? {
            val reader = BufferedReader(InputStreamReader(inputStream, Charsets.UTF_8))
            var lutSize = 0
            var is1D = false
            val domainMin = floatArrayOf(0f, 0f, 0f)
            val domainMax = floatArrayOf(1f, 1f, 1f)
            val rawFloats = FloatArray(65 * 65 * 65 * 3)
            var floatCount = 0

            var line: String?
            while (reader.readLine().also { line = it } != null) {
                val trimmed = line!!.trim()
                if (trimmed.isEmpty() || trimmed.startsWith("#")) continue

                val upper = trimmed.uppercase()
                if (upper.startsWith("TITLE")) continue
                if (upper.startsWith("LUT_3D_SIZE")) {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 2) {
                        lutSize = parts[1].toIntOrNull()?.coerceIn(2, 65) ?: 0
                        is1D = false
                    }
                    continue
                }
                if (upper.startsWith("LUT_1D_SIZE")) {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 2) {
                        lutSize = parts[1].toIntOrNull()?.coerceIn(2, 4096) ?: 0
                        is1D = true
                    }
                    continue
                }
                if (upper.startsWith("DOMAIN_MIN")) {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 4) {
                        domainMin[0] = parts[1].toFloatOrNull() ?: 0f
                        domainMin[1] = parts[2].toFloatOrNull() ?: 0f
                        domainMin[2] = parts[3].toFloatOrNull() ?: 0f
                    }
                    continue
                }
                if (upper.startsWith("DOMAIN_MAX")) {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 4) {
                        domainMax[0] = parts[1].toFloatOrNull() ?: 1f
                        domainMax[1] = parts[2].toFloatOrNull() ?: 1f
                        domainMax[2] = parts[3].toFloatOrNull() ?: 1f
                    }
                    continue
                }
                val firstCh = trimmed[0]
                if ((firstCh in '0'..'9') || firstCh == '-' || firstCh == '+' || firstCh == '.') {
                    val tok = StringTokenizer(trimmed)
                    if (tok.countTokens() >= 3 && floatCount + 3 <= rawFloats.size) {
                        val r = tok.nextToken().toFloatOrNull() ?: continue
                        val g = tok.nextToken().toFloatOrNull() ?: continue
                        val b = tok.nextToken().toFloatOrNull() ?: continue
                        rawFloats[floatCount++] = r
                        rawFloats[floatCount++] = g
                        rawFloats[floatCount++] = b
                    }
                }
            }

            if (lutSize < 2) return null
            val expectedTriples = if (is1D) lutSize else (lutSize * lutSize * lutSize)
            if (floatCount < expectedTriples * 3) return null

            val outBuf = ByteBuffer.allocateDirect(LUT_GRID_SIZE * LUT_GRID_SIZE * LUT_GRID_SIZE * 4)
                .order(ByteOrder.nativeOrder())

            val spanR = (domainMax[0] - domainMin[0]).takeIf { it > 1e-5f } ?: 1f
            val spanG = (domainMax[1] - domainMin[1]).takeIf { it > 1e-5f } ?: 1f
            val spanB = (domainMax[2] - domainMin[2]).takeIf { it > 1e-5f } ?: 1f

            // Texture layout: width = 1024 (32 slices * 32 R), height = 32 (G)
            // Row y corresponds to G = 0..31; column x corresponds to sliceB * 32 + R
            for (gIdx in 0 until LUT_GRID_SIZE) {
                val gNorm = ((gIdx.toFloat() / (LUT_GRID_SIZE - 1).toFloat()) - domainMin[1]) / spanG
                for (bIdx in 0 until LUT_GRID_SIZE) {
                    val bNorm = ((bIdx.toFloat() / (LUT_GRID_SIZE - 1).toFloat()) - domainMin[2]) / spanB
                    for (rIdx in 0 until LUT_GRID_SIZE) {
                        val rNorm = ((rIdx.toFloat() / (LUT_GRID_SIZE - 1).toFloat()) - domainMin[0]) / spanR

                        val (outR, outG, outB) = if (is1D) {
                            sample1D(rawFloats, lutSize, rNorm, gNorm, bNorm)
                        } else {
                            sample3DTrilinear(rawFloats, lutSize, rNorm, gNorm, bNorm)
                        }

                        outBuf.put((outR * 255f).roundToInt().coerceIn(0, 255).toByte())
                        outBuf.put((outG * 255f).roundToInt().coerceIn(0, 255).toByte())
                        outBuf.put((outB * 255f).roundToInt().coerceIn(0, 255).toByte())
                        outBuf.put(255.toByte())
                    }
                }
            }
            outBuf.position(0)
            return outBuf
        }

        private fun sample1D(data: FloatArray, n: Int, r: Float, g: Float, b: Float): Triple<Float, Float, Float> {
            fun interp(v: Float, ch: Int): Float {
                val pos = v.coerceIn(0f, 1f) * (n - 1)
                val i0 = floor(pos).toInt().coerceIn(0, n - 1)
                val i1 = (i0 + 1).coerceAtMost(n - 1)
                val f = pos - i0
                return data[i0 * 3 + ch] * (1f - f) + data[i1 * 3 + ch] * f
            }
            return Triple(interp(r, 0), interp(g, 1), interp(b, 2))
        }

        private fun sample3DTrilinear(
            data: FloatArray,
            n: Int,
            r: Float,
            g: Float,
            b: Float
        ): Triple<Float, Float, Float> {
            val rf = r.coerceIn(0f, 1f) * (n - 1)
            val gf = g.coerceIn(0f, 1f) * (n - 1)
            val bf = b.coerceIn(0f, 1f) * (n - 1)

            val r0 = floor(rf).toInt().coerceIn(0, n - 1)
            val g0 = floor(gf).toInt().coerceIn(0, n - 1)
            val b0 = floor(bf).toInt().coerceIn(0, n - 1)
            val r1 = (r0 + 1).coerceAtMost(n - 1)
            val g1 = (g0 + 1).coerceAtMost(n - 1)
            val b1 = (b0 + 1).coerceAtMost(n - 1)

            val dr = rf - r0
            val dg = gf - g0
            val db = bf - b0

            fun idx(ir: Int, ig: Int, ib: Int): Int = ((ib * n + ig) * n + ir) * 3

            val i000 = idx(r0, g0, b0)
            val i100 = idx(r1, g0, b0)
            val i010 = idx(r0, g1, b0)
            val i110 = idx(r1, g1, b0)
            val i001 = idx(r0, g0, b1)
            val i101 = idx(r1, g0, b1)
            val i011 = idx(r0, g1, b1)
            val i111 = idx(r1, g1, b1)

            fun ch(c: Int): Float {
                val c00 = data[i000 + c] * (1f - dr) + data[i100 + c] * dr
                val c10 = data[i010 + c] * (1f - dr) + data[i110 + c] * dr
                val c01 = data[i001 + c] * (1f - dr) + data[i101 + c] * dr
                val c11 = data[i011 + c] * (1f - dr) + data[i111 + c] * dr
                val c0 = c00 * (1f - dg) + c10 * dg
                val c1 = c01 * (1f - dg) + c11 * dg
                return (c0 * (1f - db) + c1 * db).coerceIn(0f, 1f)
            }

            return Triple(ch(0), ch(1), ch(2))
        }
    }

    private data class InputSession(
        val texId: Int,
        val surfaceTexture: SurfaceTexture,
        val surface: Surface,
        var bufWidth: Int,
        var bufHeight: Int
    )

    private val glThread = HandlerThread("EyeCamEglTee").apply { start() }
    private val glHandler = Handler(glThread.looper)

    @Volatile
    private var released = false

    // EGL / GL state (exclusively touched on glThread)
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglConfig: EGLConfig? = null
    private var pbufferSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    private var programPassthrough = 0
    private var aPositionLocPass = -1
    private var aTexCoordLocPass = -1
    private var uMvpMatrixLocPass = -1
    private var uStMatrixLocPass = -1
    private var sTextureLocPass = -1

    private var programLut = 0
    private var aPositionLocLut = -1
    private var aTexCoordLocLut = -1
    private var uMvpMatrixLocLut = -1
    private var uStMatrixLocLut = -1
    private var sTextureLocLut = -1
    private var sLutTextureLocLut = -1

    private var lutTexId = 0
    private var lutEnabled = false

    private val posBuffer: FloatBuffer = createFloatBuffer(QUAD_POSITIONS)
    private val texBuffer: FloatBuffer = createFloatBuffer(QUAD_TEX_COORDS)
    private val stMatrix = FloatArray(16)
    private val mvpMatrix = FloatArray(16)

    // Camera input sessions
    private var activeSession: InputSession? = null
    private val retiredSessions = mutableListOf<InputSession>()
    private var lastPtsNs = 0L
    private var frameParity = 0

    @Volatile
    var lowEndPreviewThrottle: Boolean = false

    // Encoder output target (exclusively touched on glThread)
    private var encoderSurfaceRef: Surface? = null
    private var encoderEglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var encoderWidth = 1920
    private var encoderHeight = 1080
    private var encoderGlCcwDeg = 0
    private var encoderMirrorX = false

    // UI preview output target (exclusively touched on glThread)
    private var previewSurfaceRef: Surface? = null
    private var previewEglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var previewWidth = 0
    private var previewHeight = 0
    private var previewGlCcwDeg = 0
    private var previewMirrorX = false

    @Volatile
    private var onFrameRendered: ((Long) -> Unit)? = null

    init {
        val latch = CountDownLatch(1)
        glHandler.post {
            try {
                initEglAndProgram()
            } catch (e: Exception) {
                Log.e(TAG, "EGL initialization failed", e)
            } finally {
                latch.countDown()
            }
        }
        latch.await(3, TimeUnit.SECONDS)
    }

    fun setOnFrameRenderedListener(listener: ((Long) -> Unit)?) {
        onFrameRendered = listener
    }

    /**
     * Uploads or clears a 32x32x32 RGBA 3D LUT texture (1024x32) on the EGL thread.
     */
    fun setLutRgba32(rgbaBuffer: ByteBuffer?) {
        if (released) return
        glHandler.post {
            if (released || eglDisplay == EGL14.EGL_NO_DISPLAY) return@post
            makePbufferCurrent()
            if (rgbaBuffer == null) {
                lutEnabled = false
                return@post
            }
            if (lutTexId == 0) {
                val ids = IntArray(1)
                GLES20.glGenTextures(1, ids, 0)
                lutTexId = ids[0]
            }
            rgbaBuffer.position(0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, lutTexId)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexImage2D(
                GLES20.GL_TEXTURE_2D,
                0,
                GLES20.GL_RGBA,
                LUT_GRID_SIZE * LUT_GRID_SIZE,
                LUT_GRID_SIZE,
                0,
                GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE,
                rgbaBuffer
            )
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0)
            lutEnabled = true
        }
    }

    /**
     * Creates a fresh [SurfaceTexture]-backed [Surface] sized for [width]x[height] and
     * delivers it asynchronously once ready on the GL thread.
     */
    fun acquireInputSurface(width: Int, height: Int, onReady: (Surface) -> Unit) {
        if (released) return
        glHandler.post {
            if (released || eglDisplay == EGL14.EGL_NO_DISPLAY) return@post
            makePbufferCurrent()

            val texId = createOesTexture()
            val st = SurfaceTexture(texId).apply {
                setDefaultBufferSize(width.coerceAtLeast(1), height.coerceAtLeast(1))
            }
            val surface = Surface(st)
            val session = InputSession(
                texId = texId,
                surfaceTexture = st,
                surface = surface,
                bufWidth = width.coerceAtLeast(1),
                bufHeight = height.coerceAtLeast(1)
            )

            activeSession?.let { previous ->
                retiredSessions.add(previous)
            }
            activeSession = session

            st.setOnFrameAvailableListener({
                if (!released && activeSession === session) {
                    renderCurrentFrame(session)
                }
            }, glHandler)

            onReady(surface)
        }
    }

    fun releaseInputSurface(surface: Surface) {
        if (released) return
        glHandler.post {
            val active = activeSession
            if (active != null && active.surface === surface) {
                destroyInputSession(active)
                activeSession = null
                return@post
            }
            val iter = retiredSessions.iterator()
            while (iter.hasNext()) {
                val item = iter.next()
                if (item.surface === surface) {
                    iter.remove()
                    destroyInputSession(item)
                    break
                }
            }
        }
    }

    fun setEncoderSurface(surface: Surface?, width: Int, height: Int, await: Boolean = false) {
        if (released && surface != null) return
        val latch = if (await) CountDownLatch(1) else null
        glHandler.post {
            try {
                if (encoderEglSurface != EGL14.EGL_NO_SURFACE) {
                    makePbufferCurrent()
                    EGL14.eglDestroySurface(eglDisplay, encoderEglSurface)
                    encoderEglSurface = EGL14.EGL_NO_SURFACE
                }
                encoderSurfaceRef = surface
                encoderWidth = width.coerceAtLeast(1)
                encoderHeight = height.coerceAtLeast(1)

                if (surface != null && surface.isValid && eglDisplay != EGL14.EGL_NO_DISPLAY && eglConfig != null) {
                    val attribs = intArrayOf(EGL14.EGL_NONE)
                    encoderEglSurface = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface, attribs, 0)
                    if (encoderEglSurface == EGL14.EGL_NO_SURFACE) {
                        Log.e(TAG, "Failed to create encoder EGLSurface: 0x${Integer.toHexString(EGL14.eglGetError())}")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "setEncoderSurface error", e)
            } finally {
                latch?.countDown()
            }
        }
        latch?.await(500, TimeUnit.MILLISECONDS)
    }

    fun setPreviewSurface(surface: Surface?, width: Int, height: Int, await: Boolean = false) {
        if (released && surface != null) return
        val latch = if (await) CountDownLatch(1) else null
        glHandler.post {
            try {
                val surfaceChanged = surface !== previewSurfaceRef
                if (surfaceChanged && previewEglSurface != EGL14.EGL_NO_SURFACE) {
                    makePbufferCurrent()
                    EGL14.eglDestroySurface(eglDisplay, previewEglSurface)
                    previewEglSurface = EGL14.EGL_NO_SURFACE
                }
                previewSurfaceRef = surface
                previewWidth = width.coerceAtLeast(0)
                previewHeight = height.coerceAtLeast(0)

                if (surfaceChanged && surface != null && surface.isValid &&
                    eglDisplay != EGL14.EGL_NO_DISPLAY && eglConfig != null
                ) {
                    val attribs = intArrayOf(EGL14.EGL_NONE)
                    previewEglSurface = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface, attribs, 0)
                    if (previewEglSurface == EGL14.EGL_NO_SURFACE) {
                        Log.e(TAG, "Failed to create preview EGLSurface: 0x${Integer.toHexString(EGL14.eglGetError())}")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "setPreviewSurface error", e)
            } finally {
                latch?.countDown()
            }
        }
        latch?.await(400, TimeUnit.MILLISECONDS)
    }

    fun updatePreviewTransform(glCcwDegrees: Int, mirrorHorizontal: Boolean) {
        if (released) return
        glHandler.post {
            previewGlCcwDeg = ((glCcwDegrees % 360) + 360) % 360
            previewMirrorX = mirrorHorizontal
        }
    }

    fun updateEncoderTransform(glCcwDegrees: Int, mirrorHorizontal: Boolean) {
        if (released) return
        glHandler.post {
            encoderGlCcwDeg = ((glCcwDegrees % 360) + 360) % 360
            encoderMirrorX = mirrorHorizontal
        }
    }

    fun release() {
        if (released) return
        released = true
        val latch = CountDownLatch(1)
        glHandler.post {
            try {
                makePbufferCurrent()
                activeSession?.let {
                    destroyInputSession(it)
                    activeSession = null
                }
                retiredSessions.forEach { destroyInputSession(it) }
                retiredSessions.clear()

                if (lutTexId != 0) {
                    GLES20.glDeleteTextures(1, intArrayOf(lutTexId), 0)
                    lutTexId = 0
                }
                if (encoderEglSurface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(eglDisplay, encoderEglSurface)
                    encoderEglSurface = EGL14.EGL_NO_SURFACE
                }
                if (previewEglSurface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(eglDisplay, previewEglSurface)
                    previewEglSurface = EGL14.EGL_NO_SURFACE
                }
                if (programPassthrough != 0) {
                    GLES20.glDeleteProgram(programPassthrough)
                    programPassthrough = 0
                }
                if (programLut != 0) {
                    GLES20.glDeleteProgram(programLut)
                    programLut = 0
                }
                if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                    if (pbufferSurface != EGL14.EGL_NO_SURFACE) {
                        EGL14.eglDestroySurface(eglDisplay, pbufferSurface)
                        pbufferSurface = EGL14.EGL_NO_SURFACE
                    }
                    if (eglContext != EGL14.EGL_NO_CONTEXT) {
                        EGL14.eglDestroyContext(eglDisplay, eglContext)
                        eglContext = EGL14.EGL_NO_CONTEXT
                    }
                    EGL14.eglTerminate(eglDisplay)
                    eglDisplay = EGL14.EGL_NO_DISPLAY
                }
            } catch (e: Exception) {
                Log.e(TAG, "release error", e)
            } finally {
                latch.countDown()
            }
        }
        latch.await(1, TimeUnit.SECONDS)
        glThread.quitSafely()
    }

    private fun renderCurrentFrame(session: InputSession) {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) return
        makePbufferCurrent()
        try {
            session.surfaceTexture.updateTexImage()
        } catch (e: Exception) {
            return
        }
        session.surfaceTexture.getTransformMatrix(stMatrix)

        val rawPtsNs = session.surfaceTexture.timestamp
        val nowNs = System.nanoTime()
        val candidateNs = if (rawPtsNs > 0L) rawPtsNs else nowNs
        val ptsNs = if (candidateNs > lastPtsNs) candidateNs else lastPtsNs + 1_000_000L
        lastPtsNs = ptsNs
        frameParity = (frameParity + 1) and 0x7FFF

        val encSurf = encoderSurfaceRef
        val encoderRendered = if (encSurf != null && encSurf.isValid && encoderEglSurface != EGL14.EGL_NO_SURFACE) {
            drawToSurface(
                eglSurface = encoderEglSurface,
                texId = session.texId,
                bufW = session.bufWidth,
                bufH = session.bufHeight,
                dstW = encoderWidth,
                dstH = encoderHeight,
                glCcwDeg = encoderGlCcwDeg,
                desiredMirrorX = encoderMirrorX,
                ptsNs = ptsNs
            )
            true
        } else {
            false
        }

        // On budget/legacy devices when encoder stream is active, render local UI preview every 2nd frame
        val skipUiPreview = lowEndPreviewThrottle && encoderRendered && (frameParity and 1) == 1
        val prevSurf = previewSurfaceRef
        if (!skipUiPreview && prevSurf != null && prevSurf.isValid &&
            previewEglSurface != EGL14.EGL_NO_SURFACE && previewWidth > 0 && previewHeight > 0
        ) {
            drawToSurface(
                eglSurface = previewEglSurface,
                texId = session.texId,
                bufW = session.bufWidth,
                bufH = session.bufHeight,
                dstW = previewWidth,
                dstH = previewHeight,
                glCcwDeg = previewGlCcwDeg,
                desiredMirrorX = previewMirrorX,
                ptsNs = null
            )
        }

        onFrameRendered?.invoke(ptsNs)
    }

    private fun drawToSurface(
        eglSurface: EGLSurface,
        texId: Int,
        bufW: Int,
        bufH: Int,
        dstW: Int,
        dstH: Int,
        glCcwDeg: Int,
        desiredMirrorX: Boolean,
        ptsNs: Long?
    ) {
        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            return
        }

        val actualDstW = dstW
        val actualDstH = dstH
        if (actualDstW <= 0 || actualDstH <= 0) return

        GLES20.glDisable(GLES20.GL_CULL_FACE)
        GLES20.glViewport(0, 0, actualDstW, actualDstH)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

        val rawSpanW = hypot(stMatrix[0] * bufW.toFloat(), stMatrix[1] * bufH.toFloat())
        val rawSpanH = hypot(stMatrix[4] * bufW.toFloat(), stMatrix[5] * bufH.toFloat())
        val srcW = if (rawSpanW > 1f) rawSpanW else bufW.toFloat().coerceAtLeast(1f)
        val srcH = if (rawSpanH > 1f) rawSpanH else bufH.toFloat().coerceAtLeast(1f)
        val stAlreadyMirrored = (stMatrix[0] * stMatrix[5] - stMatrix[1] * stMatrix[4]) > 0f

        val rotCcw = ((glCcwDeg % 360) + 360) % 360
        val imgW = if (rotCcw % 180 == 0) srcW else srcH
        val imgH = if (rotCcw % 180 == 0) srcH else srcW
        val fitScale = min(actualDstW.toFloat() / imgW, actualDstH.toFloat() / imgH)
        val applyFlipX = (stAlreadyMirrored != desiredMirrorX)

        Matrix.setIdentityM(mvpMatrix, 0)
        Matrix.scaleM(mvpMatrix, 0, fitScale / actualDstW.toFloat(), fitScale / actualDstH.toFloat(), 1f)
        if (applyFlipX) {
            Matrix.scaleM(mvpMatrix, 0, -1f, 1f, 1f)
        }
        if (rotCcw != 0) {
            Matrix.rotateM(mvpMatrix, 0, rotCcw.toFloat(), 0f, 0f, 1f)
        }
        Matrix.scaleM(mvpMatrix, 0, srcW, srcH, 1f)

        val useLut = lutEnabled && lutTexId != 0 && programLut != 0
        val activeProg = if (useLut) programLut else programPassthrough
        val posLoc = if (useLut) aPositionLocLut else aPositionLocPass
        val texLoc = if (useLut) aTexCoordLocLut else aTexCoordLocPass
        val mvpLoc = if (useLut) uMvpMatrixLocLut else uMvpMatrixLocPass
        val stLoc = if (useLut) uStMatrixLocLut else uStMatrixLocPass
        val oesSamplerLoc = if (useLut) sTextureLocLut else sTextureLocPass

        GLES20.glUseProgram(activeProg)
        GLES20.glUniformMatrix4fv(mvpLoc, 1, false, mvpMatrix, 0)
        GLES20.glUniformMatrix4fv(stLoc, 1, false, stMatrix, 0)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glUniform1i(oesSamplerLoc, 0)

        if (useLut) {
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, lutTexId)
            GLES20.glUniform1i(sLutTextureLocLut, 1)
        }

        posBuffer.position(0)
        GLES20.glEnableVertexAttribArray(posLoc)
        GLES20.glVertexAttribPointer(posLoc, 2, GLES20.GL_FLOAT, false, 0, posBuffer)

        texBuffer.position(0)
        GLES20.glEnableVertexAttribArray(texLoc)
        GLES20.glVertexAttribPointer(texLoc, 2, GLES20.GL_FLOAT, false, 0, texBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(posLoc)
        GLES20.glDisableVertexAttribArray(texLoc)
        if (useLut) {
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        }
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0)

        if (ptsNs != null) {
            EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, ptsNs)
        }
        EGL14.eglSwapBuffers(eglDisplay, eglSurface)
    }

    private fun initEglAndProgram() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        require(eglDisplay != EGL14.EGL_NO_DISPLAY) { "Unable to get EGL14 display" }
        val version = IntArray(2)
        require(EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) { "Unable to initialize EGL14" }

        eglConfig = chooseEglConfig(recordable = true) ?: chooseEglConfig(recordable = false)
        requireNotNull(eglConfig) { "Unable to find suitable EGLConfig" }

        val ctxAttribs = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, ctxAttribs, 0)
        require(eglContext != EGL14.EGL_NO_CONTEXT) { "Unable to create EGLContext" }

        val pbufAttribs = intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE)
        pbufferSurface = EGL14.eglCreatePbufferSurface(eglDisplay, eglConfig, pbufAttribs, 0)
        require(pbufferSurface != EGL14.EGL_NO_SURFACE) { "Unable to create pbuffer surface" }

        makePbufferCurrent()

        programPassthrough = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER_PASSTHROUGH)
        aPositionLocPass = GLES20.glGetAttribLocation(programPassthrough, "aPosition")
        aTexCoordLocPass = GLES20.glGetAttribLocation(programPassthrough, "aTexCoord")
        uMvpMatrixLocPass = GLES20.glGetUniformLocation(programPassthrough, "uMvpMatrix")
        uStMatrixLocPass = GLES20.glGetUniformLocation(programPassthrough, "uStMatrix")
        sTextureLocPass = GLES20.glGetUniformLocation(programPassthrough, "sTexture")

        try {
            programLut = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER_LUT3D)
            aPositionLocLut = GLES20.glGetAttribLocation(programLut, "aPosition")
            aTexCoordLocLut = GLES20.glGetAttribLocation(programLut, "aTexCoord")
            uMvpMatrixLocLut = GLES20.glGetUniformLocation(programLut, "uMvpMatrix")
            uStMatrixLocLut = GLES20.glGetUniformLocation(programLut, "uStMatrix")
            sTextureLocLut = GLES20.glGetUniformLocation(programLut, "sTexture")
            sLutTextureLocLut = GLES20.glGetUniformLocation(programLut, "sLutTexture")
        } catch (e: Exception) {
            Log.w(TAG, "3D LUT shader compilation skipped: ${e.message}")
            programLut = 0
        }
    }

    private fun chooseEglConfig(recordable: Boolean): EGLConfig? {
        val base = mutableListOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT
        )
        if (recordable) {
            base.add(EGL_RECORDABLE_ANDROID)
            base.add(1)
        }
        base.add(EGL14.EGL_NONE)
        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfigs = IntArray(1)
        val ok = EGL14.eglChooseConfig(eglDisplay, base.toIntArray(), 0, configs, 0, configs.size, numConfigs, 0)
        return if (ok && numConfigs[0] > 0) configs[0] else null
    }

    private fun makePbufferCurrent() {
        if (eglDisplay != EGL14.EGL_NO_DISPLAY && pbufferSurface != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(eglDisplay, pbufferSurface, pbufferSurface, eglContext)
        }
    }

    private fun createOesTexture(): Int {
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        val texId = ids[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0)
        return texId
    }

    private fun destroyInputSession(session: InputSession) {
        try {
            session.surfaceTexture.setOnFrameAvailableListener(null)
            session.surface.release()
            session.surfaceTexture.release()
            GLES20.glDeleteTextures(1, intArrayOf(session.texId), 0)
        } catch (e: Exception) {
            Log.w(TAG, "Error releasing input session: ${e.message}")
        }
    }

    private fun buildProgram(vertexSrc: String, fragmentSrc: String): Int {
        val vs = compileShader(GLES20.GL_VERTEX_SHADER, vertexSrc)
        val fs = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSrc)
        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vs)
        GLES20.glAttachShader(prog, fs)
        GLES20.glLinkProgram(prog)
        val status = IntArray(1)
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, status, 0)
        GLES20.glDeleteShader(vs)
        GLES20.glDeleteShader(fs)
        if (status[0] == 0) {
            val info = GLES20.glGetProgramInfoLog(prog)
            GLES20.glDeleteProgram(prog)
            error("GL program link failed: $info")
        }
        return prog
    }

    private fun compileShader(type: Int, src: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, src)
        GLES20.glCompileShader(shader)
        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            val info = GLES20.glGetShaderInfoLog(shader)
            GLES20.glDeleteShader(shader)
            error("Shader compile failed: $info")
        }
        return shader
    }

    private fun createFloatBuffer(data: FloatArray): FloatBuffer =
        ByteBuffer.allocateDirect(data.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply {
                put(data)
                position(0)
            }
}
