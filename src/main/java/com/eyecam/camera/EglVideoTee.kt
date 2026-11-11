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
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.min

/**
 * Zero-copy OpenGL ES 2.0 video tee.
 *
 * Receives camera frames from a single CameraX [Preview] SurfaceRequest via an OES
 * [SurfaceTexture], and renders each frame independently on a dedicated EGL thread to:
 *  1. The hardware H.264 encoder input [Surface] (landscape/auto-rotated stream with monotonic PTS).
 *  2. The optional UI [TextureView] [Surface] (FIT_CENTER letterboxed, zero stretching).
 */
class EglVideoTee {

    companion object {
        private const val TAG = "EyeCam-EglTee"
        private const val EGL_RECORDABLE_ANDROID = 0x3142

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

        private const val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES sTexture;
            void main() {
                gl_FragColor = texture2D(sTexture, vTexCoord);
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

    private var program = 0
    private var aPositionLoc = -1
    private var aTexCoordLoc = -1
    private var uMvpMatrixLoc = -1
    private var uStMatrixLoc = -1
    private var sTextureLoc = -1

    private val posBuffer: FloatBuffer = createFloatBuffer(QUAD_POSITIONS)
    private val texBuffer: FloatBuffer = createFloatBuffer(QUAD_TEX_COORDS)
    private val stMatrix = FloatArray(16)
    private val mvpMatrix = FloatArray(16)

    // Camera input sessions
    private var activeSession: InputSession? = null
    private val retiredSessions = mutableListOf<InputSession>()
    private var lastPtsNs = 0L

    // Encoder output target (exclusively touched on glThread)
    private var encoderSurfaceRef: Surface? = null
    private var encoderEglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var encoderWidth = 1920
    private var encoderHeight = 1080
    private var encoderCwDeg = 0
    private var encoderMirrorX = false

    // UI preview output target (exclusively touched on glThread)
    private var previewSurfaceRef: Surface? = null
    private var previewEglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var previewWidth = 0
    private var previewHeight = 0
    private var previewCwDeg = 0
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

    /**
     * Releases a previously acquired input [Surface] after CameraX completes the SurfaceRequest.
     */
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

    /**
     * Attaches or detaches the MediaCodec encoder input [Surface].
     * When [await] is true, blocks briefly until any pending draw to the old surface finishes.
     */
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

    /**
     * Attaches, resizes, or detaches the UI [TextureView] preview [Surface].
     * When [await] is true (e.g., during SurfaceTexture destruction), blocks until destroyed.
     */
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

    fun updatePreviewTransform(cwDegrees: Int, mirrorHorizontal: Boolean) {
        if (released) return
        glHandler.post {
            previewCwDeg = ((cwDegrees % 360) + 360) % 360
            previewMirrorX = mirrorHorizontal
        }
    }

    fun updateEncoderTransform(cwDegrees: Int, mirrorHorizontal: Boolean) {
        if (released) return
        glHandler.post {
            encoderCwDeg = ((cwDegrees % 360) + 360) % 360
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

                if (encoderEglSurface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(eglDisplay, encoderEglSurface)
                    encoderEglSurface = EGL14.EGL_NO_SURFACE
                }
                if (previewEglSurface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(eglDisplay, previewEglSurface)
                    previewEglSurface = EGL14.EGL_NO_SURFACE
                }
                if (program != 0) {
                    GLES20.glDeleteProgram(program)
                    program = 0
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

        val encSurf = encoderSurfaceRef
        if (encSurf != null && encSurf.isValid && encoderEglSurface != EGL14.EGL_NO_SURFACE) {
            drawToSurface(
                eglSurface = encoderEglSurface,
                bufW = session.bufWidth,
                bufH = session.bufHeight,
                dstW = encoderWidth,
                dstH = encoderHeight,
                cwDeg = encoderCwDeg,
                mirrorX = encoderMirrorX,
                ptsNs = ptsNs
            )
        }

        val prevSurf = previewSurfaceRef
        if (prevSurf != null && prevSurf.isValid && previewEglSurface != EGL14.EGL_NO_SURFACE &&
            previewWidth > 0 && previewHeight > 0
        ) {
            drawToSurface(
                eglSurface = previewEglSurface,
                bufW = session.bufWidth,
                bufH = session.bufHeight,
                dstW = previewWidth,
                dstH = previewHeight,
                cwDeg = previewCwDeg,
                mirrorX = previewMirrorX,
                ptsNs = null
            )
        }

        onFrameRendered?.invoke(ptsNs)
    }

    private fun drawToSurface(
        eglSurface: EGLSurface,
        bufW: Int,
        bufH: Int,
        dstW: Int,
        dstH: Int,
        cwDeg: Int,
        mirrorX: Boolean,
        ptsNs: Long?
    ) {
        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            return
        }
        GLES20.glViewport(0, 0, dstW, dstH)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

        val imgW = if (cwDeg % 180 == 0) bufW.toFloat() else bufH.toFloat()
        val imgH = if (cwDeg % 180 == 0) bufH.toFloat() else bufW.toFloat()
        val fitScale = min(dstW.toFloat() / imgW, dstH.toFloat() / imgH)

        Matrix.setIdentityM(mvpMatrix, 0)
        Matrix.scaleM(mvpMatrix, 0, fitScale / dstW.toFloat(), fitScale / dstH.toFloat(), 1f)
        if (mirrorX) {
            Matrix.scaleM(mvpMatrix, 0, -1f, 1f, 1f)
        }
        if (cwDeg != 0) {
            Matrix.rotateM(mvpMatrix, 0, -cwDeg.toFloat(), 0f, 0f, 1f)
        }
        Matrix.scaleM(mvpMatrix, 0, bufW.toFloat(), bufH.toFloat(), 1f)

        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(uMvpMatrixLoc, 1, false, mvpMatrix, 0)
        GLES20.glUniformMatrix4fv(uStMatrixLoc, 1, false, stMatrix, 0)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, activeSession?.texId ?: 0)
        GLES20.glUniform1i(sTextureLoc, 0)

        posBuffer.position(0)
        GLES20.glEnableVertexAttribArray(aPositionLoc)
        GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 0, posBuffer)

        texBuffer.position(0)
        GLES20.glEnableVertexAttribArray(aTexCoordLoc)
        GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 0, texBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(aPositionLoc)
        GLES20.glDisableVertexAttribArray(aTexCoordLoc)
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

        program = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
        aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")
        uMvpMatrixLoc = GLES20.glGetUniformLocation(program, "uMvpMatrix")
        uStMatrixLoc = GLES20.glGetUniformLocation(program, "uStMatrix")
        sTextureLoc = GLES20.glGetUniformLocation(program, "sTexture")
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
