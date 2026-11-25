package com.plyg.airlink

import android.Manifest
import android.content.Intent
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Matrix
import android.os.Bundle
import android.os.SystemClock
import android.util.Size
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.gesturerecognizer.GestureRecognizer
import java.util.concurrent.Executors
import kotlin.math.hypot

/** Opt-in, on-device camera recognition. No frame is saved or sent across the network. */
class HandGestureActivity : ComponentActivity() {
    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) startCamera() else status.text = "Kamera izni olmadan el hareketi tanınamaz."
    }
    private val cameraExecutor = Executors.newSingleThreadExecutor()
    private lateinit var preview: PreviewView
    private lateinit var status: TextView
    private var provider: ProcessCameraProvider? = null
    private var recognizer: GestureRecognizer? = null
    private var lastFrameAt = 0L
    private var candidate = ""
    private var candidateAt = 0L
    private var heldAt = 0L
    private var palmFirstAt = 0L
    private var palmLastAt = 0L
    private var palmCount = 0
    private var cooldownUntil = 0L
    private var pullMode = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = FrameLayout(this)
        preview = PreviewView(this).apply { scaleType = PreviewView.ScaleType.FILL_CENTER }
        root.addView(preview, FrameLayout.LayoutParams(-1, -1))
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
            setBackgroundColor(0xAA101010.toInt())
        }
        status = TextView(this).apply {
            text = "Kameraya yumruğunuzu gösterin; tuttuğunuzda avucunuzu açarak gönderin."
            textSize = 18f
            setTextColor(0xFFFFFFFF.toInt())
        }
        controls.addView(status)
        controls.addView(Button(this).apply {
            text = "Yön: Telefondan gönder"
            setOnClickListener {
                pullMode = !pullMode
                text = if (pullMode) "Yön: Bilgisayardan al" else "Yön: Telefondan gönder"
                status.text = if (pullMode) "Bilgisayarda dosya veya panoyu hazırlayın. Sonra yumruk yapıp avucunuzu açın."
                    else "Paylaşılan dosyaları veya telefon panosunu göndermek için yumruk yapıp avucunuzu açın."
                heldAt = 0L
                candidate = ""
                palmCount = 0
            }
        })
        controls.addView(Button(this).apply {
            text = "Kapat"
            setOnClickListener { closeToAirLink() }
        })
        root.addView(controls, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
        setContentView(root)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = closeToAirLink()
        })

        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) startCamera()
        else requestCamera.launch(Manifest.permission.CAMERA)
    }

    private fun startCamera() {
        try {
            val options = GestureRecognizer.GestureRecognizerOptions.builder()
                .setBaseOptions(BaseOptions.builder().setModelAssetPath("gesture_recognizer.task").build())
                .setRunningMode(RunningMode.LIVE_STREAM)
                .setNumHands(1)
                .setResultListener { result, _ ->
                    val category = result.gestures().firstOrNull()?.firstOrNull()
                    val hand = result.landmarks().firstOrNull()
                    val openByJoints = if (hand == null || hand.size < 21) false else {
                        val wrist = hand[0]
                        listOf(8 to 6, 12 to 10, 16 to 14, 20 to 18).count { (tip, pip) ->
                            val tipDistance = hypot((hand[tip].x() - wrist.x()).toDouble(),
                                (hand[tip].y() - wrist.y()).toDouble())
                            val pipDistance = hypot((hand[pip].x() - wrist.x()).toDouble(),
                                (hand[pip].y() - wrist.y()).toDouble())
                            tipDistance > pipDistance * 1.15
                        } >= 3
                    }
                    runOnUiThread { onGesture(category?.categoryName().orEmpty(),
                        category?.score() ?: 0f, openByJoints) }
                }
                .setErrorListener { error -> runOnUiThread { status.text = "Tanıma hatası: ${error.message}" } }
                .build()
            recognizer = GestureRecognizer.createFromOptions(this, options)
        } catch (error: Exception) {
            status.text = "El tanıma başlatılamadı: ${error.message}"
            return
        }

        val future = ProcessCameraProvider.getInstance(this)
        future.addListener({
            try {
                val camera = future.get()
                provider = camera
                val view = Preview.Builder().build().also { it.surfaceProvider = preview.surfaceProvider }
                val analysis = ImageAnalysis.Builder()
                    .setResolutionSelector(ResolutionSelector.Builder()
                        .setResolutionStrategy(ResolutionStrategy(Size(640, 480),
                            ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER))
                        .build())
                    .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888)
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .build()
                analysis.setAnalyzer(cameraExecutor, ::analyze)
                camera.unbindAll()
                camera.bindToLifecycle(this, CameraSelector.DEFAULT_FRONT_CAMERA, view, analysis)
            } catch (error: Exception) {
                status.text = "Ön kamera açılamadı: ${error.message}"
            }
        }, ContextCompat.getMainExecutor(this))
    }

    private fun analyze(frame: ImageProxy) {
        try {
            val now = SystemClock.uptimeMillis()
            if (now - lastFrameAt < 100) return
            lastFrameAt = now
            val bitmap = Bitmap.createBitmap(frame.width, frame.height, Bitmap.Config.ARGB_8888)
            bitmap.copyPixelsFromBuffer(frame.planes[0].buffer)
            val rotation = frame.imageInfo.rotationDegrees
            val upright = if (rotation == 0) bitmap else Bitmap.createBitmap(bitmap, 0, 0,
                bitmap.width, bitmap.height, Matrix().apply { postRotate(rotation.toFloat()) }, true)
            recognizer?.recognizeAsync(BitmapImageBuilder(upright).build(), now)
        } catch (error: Exception) {
            runOnUiThread { status.text = "Kamera karesi işlenemedi: ${error.message}" }
        } finally {
            frame.close()
        }
    }

    private fun onGesture(name: String, score: Float, openByJoints: Boolean) {
        val now = SystemClock.uptimeMillis()
        if (now < cooldownUntil) return
        if (heldAt != 0L && now - heldAt > 12000) {
            heldAt = 0
            palmCount = 0
            status.text = "Tutma süresi doldu. Yeniden yumruk yapın."
        }
        if (heldAt == 0L) {
            val fist = name == "Closed_Fist" && score >= 0.75f
            if (!fist) {
                candidate = ""
                return
            }
            if (candidate != "Closed_Fist") {
                candidate = "Closed_Fist"
                candidateAt = now
            } else if (now - candidateAt >= 300) {
                heldAt = now
                palmCount = 0
                status.text = "Tutuldu. Şimdi avucunuzu açın."
            }
            return
        }
        // Open_Palm is scored lower on this phone and often alternates with "None".
        // Extended finger joints provide a second signal; require two nearby observations.
        if ((name == "Open_Palm" && score >= 0.50f) || openByJoints) {
            if (now - palmLastAt > 1000) {
                palmFirstAt = now
                palmCount = 0
            }
            palmLastAt = now
            palmCount++
            if (palmCount < 2 || now - palmFirstAt < 100) return
            heldAt = 0
            palmCount = 0
            cooldownUntil = now + 3000
            candidate = ""
            status.text = "Gönderiliyor…"
            val sent = if (pullMode) MainActivity.nativeReady && nativeRequestGesturePull()
                else {
                    val text = try {
                        getSystemService(ClipboardManager::class.java)?.primaryClip
                            ?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)?.toString().orEmpty()
                    } catch (_: Exception) { "" }
                    MainActivity.nativeReady && nativeHandReleased(text)
                }
            status.text = if (sent) {
                if (pullMode) "Bilgisayardaki hazır aktarım istendi. Gelen dosyayı onaylayın."
                else "Gönderme isteği iletildi. Yeniden yumruk yapabilirsiniz."
            } else "İstek gönderilemedi: çevrimiçi bilgisayarı ve paylaşılacak içeriği kontrol edin."
            if (sent && pullMode) closeToAirLink()  // Reveal the incoming file offer or clipboard.
        }
    }

    private fun closeToAirLink() {
        startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
        finish()
    }

    override fun onDestroy() {
        provider?.unbindAll()
        recognizer?.close()
        cameraExecutor.shutdown()
        super.onDestroy()
    }

    companion object {
        @JvmStatic private external fun nativeHandReleased(clipboardText: String): Boolean
        @JvmStatic private external fun nativeRequestGesturePull(): Boolean
    }
}
