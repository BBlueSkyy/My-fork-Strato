package org.stratoemu.strato.views

import android.content.Context
import android.util.AttributeSet
import android.util.Rational
import android.view.SurfaceView
import kotlin.math.roundToInt

class FixedRatioSurfaceView @JvmOverloads constructor(context : Context, attrs : AttributeSet? = null, defStyleAttr : Int = 0) : SurfaceView(context, attrs, defStyleAttr) {
    private var aspectRatio : Float = 0f // (width / height), 0f is a special value for stretch
    private var balancedFullscreen = false

    /**
     * Sets the desired aspect ratio for this view
     * @param ratio the ratio to force the view to, or null to stretch to fit
     */
    fun setAspectRatio(ratio : Rational?) {
        balancedFullscreen = false
        aspectRatio = ratio?.toFloat() ?: 0f
        scaleX = 1f
        requestLayout()
    }

    /**
     * Fills wide displays using a 16:9 guest surface, with a 19:9 visible
     * image. Horizontal scaling is reduced compared to device-ratio stretch;
     * the remaining difference is cropped evenly from the top and bottom.
     */
    fun setBalancedFullscreen() {
        balancedFullscreen = true
        aspectRatio = 16f / 9f
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec : Int, heightMeasureSpec : Int) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val height = MeasureSpec.getSize(heightMeasureSpec)
        if (balancedFullscreen && width > 0 && height > 0 && width.toFloat() / height > 16f / 9f) {
            // Keep the Vulkan surface at 16:9. Scale its visible width to
            // cover the display, cropping vertically beyond 19:9.
            val surfaceHeight = maxOf(height.toFloat(), width / (19f / 9f)).roundToInt()
            val surfaceWidth = (surfaceHeight * (16f / 9f)).roundToInt()
            setMeasuredDimension(surfaceWidth, surfaceHeight)
            scaleX = width.toFloat() / surfaceWidth
            return
        }

        scaleX = 1f
        if (aspectRatio != 0f) {
            val newWidth : Int
            val newHeight : Int
            if (height * aspectRatio < width) {
                newWidth = (height * aspectRatio).roundToInt()
                newHeight = height
            } else {
                newWidth = width
                newHeight = (width / aspectRatio).roundToInt()
            }
            setMeasuredDimension(newWidth, newHeight)
        } else {
            setMeasuredDimension(width, height)
        }
    }
}
