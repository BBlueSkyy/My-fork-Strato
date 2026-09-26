/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.input

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.*
import androidx.core.content.getSystemService
import org.stratoemu.strato.settings.EmulationSettings
import org.stratoemu.strato.utils.ByteBufferSerializable
import org.stratoemu.strato.utils.u64
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs

/**
 * Handles input events during emulation
 */
class InputHandler(private val inputManager : InputManager, private val emulationSettings : EmulationSettings) : SensorEventListener {
    companion object {
        /**
         * This initializes a guest controller in libskyline
         *
         * @param index The arbitrary index of the controller, this is to handle matching with a partner Joy-Con
         * @param type The type of the host controller
         * @param partnerIndex The index of a partner Joy-Con if there is one
         * @note This is blocking and will stall till input has been initialized on the guest
         */
        external fun setController(index : Int, type : Int, partnerIndex : Int = -1)

        /**
         * This flushes the controller updates on the guest
         *
         * @note This is blocking and will stall till input has been initialized on the guest
         */
        external fun updateControllers()

        /**
         * This sets the state of the buttons specified in the mask on a specific controller
         *
         * @param index The index of the controller this is directed to
         * @param mask The mask of the button that are being set
         * @param pressed If the buttons are being pressed or released
         */
        external fun setButtonState(index : Int, mask : Long, pressed : Boolean)

        /**
         * This sets the value of a specific axis on a specific controller
         *
         * @param index The index of the controller this is directed to
         * @param axis The ID of the axis that is being modified
         * @param value The value to set the axis to
         */
        external fun setAxisValue(index : Int, axis : Int, value : Int)

        /** Diagnostic-only tracing for the origin of axis updates. */
        external fun traceAxisSource(source : Int, index : Int, axis : Int, value : Int)

        /**
         * This sets the values of the motion sensor on a specific controller
         *
         * @param index The index of the controller this is directed to
         * @param motionId The ID of the motion sensor that is being modified
         * @param value A byte buffer of skyline::input::MotionInput in C++
         */
        private external fun setMotionState(index : Int, motionId : Int, value : ByteBuffer)

        /**
         * This sets the values of the points on the guest touch-screen
         *
         * @param points An array of skyline::input::TouchScreenPoint in C++ represented as integers
         */
        external fun setTouchState(points : IntArray)

        /** Updates one USB HID keyboard usage and the accompanying modifier state. */
        private external fun setKeyboardState(usage : Int, pressed : Boolean, modifiers : Int)

        /** Updates the physical mouse state exposed through HID shared memory. */
        private external fun setMouseState(x : Int, y : Int, deltaX : Int, deltaY : Int, wheelX : Int, wheelY : Int, buttons : Int)

        /**
         * Minimum dead zone floor applied to analog axes, used as a fallback when the host
         * driver reports an unreliable (e.g. zero) `flat` value, or reports no [InputDevice.MotionRange]
         * at all for a given axis/source combination. Without this floor, idle stick noise on
         * affected devices can be read as a constant, unfiltered analog input.
         */
        private const val MIN_AXIS_DEAD_ZONE = 0.15f
    }

    @Suppress("ArrayInDataClass")
    data class MotionSensorInput(
        var timestamp : u64 = 0uL,
        var deltaTimestamp : u64 = 0uL,
        @param:ByteBufferSerializable.ByteBufferSerializableArray(3) var gyroscope : FloatArray = FloatArray(3),
        @param:ByteBufferSerializable.ByteBufferSerializableArray(3) var accelerometer : FloatArray = FloatArray(3),
        @param:ByteBufferSerializable.ByteBufferSerializableArray(4) var quaternion : FloatArray = FloatArray(4),
        @param:ByteBufferSerializable.ByteBufferSerializableArray(9) var orientationMatrix : FloatArray = FloatArray(9),
    ) : ByteBufferSerializable

    /**
     * The latest state of the motion sensor
     */
    private val motionSensor = MotionSensorInput()

    /**
     * Buffer for passing motion data to c++
     */
    private val motionDataBufferSize = 0x5C
    private val motionDataBuffer = ByteBuffer.allocateDirect(motionDataBufferSize).order(ByteOrder.LITTLE_ENDIAN)

    /**
     * Used for adjusting motion to phone orientation
     */
    private val motionRotationMatrix = FloatArray(9)
    private val motionGyroOrientation : FloatArray = FloatArray(3)
    private val motionAcelOrientation : FloatArray = FloatArray(3)
    private var motionAxisOrientationX = SensorManager.AXIS_Y
    private var motionAxisOrientationY = SensorManager.AXIS_X

    /**
     * Initializes all of the controllers from [InputManager] on the guest
     */
    fun initializeControllers() {
        for (controller in inputManager.controllers.values) {
            if (controller.type != ControllerType.None) {
                val type = when (controller.type) {
                    ControllerType.None -> throw IllegalArgumentException()
                    ControllerType.HandheldProController -> if (emulationSettings.isDocked) ControllerType.ProController.id else ControllerType.HandheldProController.id
                    ControllerType.ProController, ControllerType.JoyConLeft, ControllerType.JoyConRight -> controller.type.id
                }

                val partnerIndex = when (controller) {
                    is JoyConLeftController -> controller.partnerId
                    is JoyConRightController -> controller.partnerId
                    else -> null
                }

                setController(controller.id, type, partnerIndex ?: -1)
            }
        }

        updateControllers()
    }

    fun initialiseMotionSensors(context : Context) {
        val sensorManager = context.getSystemService<SensorManager>() ?: return
        val sensorList = sensorManager.getSensorList(Sensor.TYPE_ALL)
        val hasRotationVector = sensorList.any { sensor -> sensor.type == Sensor.TYPE_ROTATION_VECTOR }

        sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.also { accelerometer ->
            sensorManager.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_GAME)
        }
        sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.also { gyroscope ->
            sensorManager.registerListener(this, gyroscope, SensorManager.SENSOR_DELAY_GAME)
        }
        sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)?.also { rotationVector ->
            sensorManager.registerListener(this, rotationVector, SensorManager.SENSOR_DELAY_GAME)
        }
        // Avoid listening to two rotation vectors at once
        if (!hasRotationVector) {
            sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)?.also { rotationVector ->
                sensorManager.registerListener(this, rotationVector, SensorManager.SENSOR_DELAY_GAME)
            }
        }

        setMotionOrientation90()
        val orientationEventListener = object : OrientationEventListener(context) {
            override fun onOrientationChanged(orientation : Int) {
                when {
                    isWithinOrientationRange(orientation, 270) -> {
                        setMotionOrientation270()
                    }
                    isWithinOrientationRange(orientation, 90) -> {
                        setMotionOrientation90()
                    }
                }
            }

            private fun isWithinOrientationRange(
                currentOrientation : Int, targetOrientation : Int, epsilon : Int = 90
            ) : Boolean {
                return currentOrientation > targetOrientation - epsilon
                        && currentOrientation < targetOrientation + epsilon
            }
        }
        orientationEventListener.enable()
    }

    /**
     * Configures motion axis to a 90° angle
     */
    fun setMotionOrientation90() {
        motionGyroOrientation[0] = 1.0f
        motionGyroOrientation[1] = -1.0f
        motionGyroOrientation[2] = 1.0f
        motionAcelOrientation[0] = -1.0f
        motionAcelOrientation[1] = 1.0f
        motionAcelOrientation[2] = -1.0f
        motionAxisOrientationX = SensorManager.AXIS_Y
        motionAxisOrientationY = SensorManager.AXIS_X
    }

    /**
     * Configures motion axis to a 270° angle
     */
    fun setMotionOrientation270() {
        motionGyroOrientation[0] = -1.0f
        motionGyroOrientation[1] = 1.0f
        motionGyroOrientation[2] = 1.0f
        motionAcelOrientation[0] = 1.0f
        motionAcelOrientation[1] = -1.0f
        motionAcelOrientation[2] = -1.0f

        // TODO: Find the correct configuration here
        motionAxisOrientationX = SensorManager.AXIS_Y
        motionAxisOrientationY = SensorManager.AXIS_X
    }

    /**
     * Handles translating any [KeyHostEvent]s to a [GuestEvent] that is passed into libskyline
     */
    fun handleKeyEvent(event : KeyEvent) : Boolean {
        if (event.repeatCount != 0)
            return false

        val action = when (event.action) {
            KeyEvent.ACTION_DOWN -> ButtonState.Pressed
            KeyEvent.ACTION_UP -> ButtonState.Released
            else -> return false
        }

        when (val guestEvent = inputManager.eventMap[KeyHostEvent(event.device.descriptor, event.keyCode)]) {
            is ButtonGuestEvent -> {
                if (guestEvent.button != ButtonId.Menu)
                    setButtonState(guestEvent.id, guestEvent.button.value, action.state)
                return true
            }

            is AxisGuestEvent -> {
                val axisValue = (if (action == ButtonState.Pressed) if (guestEvent.polarity) Short.MAX_VALUE else Short.MIN_VALUE else 0).toInt()
                traceAxisSource(1, guestEvent.id, guestEvent.axis.ordinal, axisValue)
                setAxisValue(guestEvent.id, guestEvent.axis.ordinal, axisValue)
                return true
            }

            else -> {}
        }

        val usage = androidKeyCodeToHidUsage(event.keyCode)
        if (usage != null && event.isFromSource(InputDevice.SOURCE_KEYBOARD)) {
            setKeyboardState(usage, action.state, keyboardModifiers(event.metaState))
            return true
        }

        return false
    }

    private fun keyboardModifiers(metaState : Int) : Int {
        var modifiers = 0
        if (metaState and KeyEvent.META_CTRL_ON != 0) modifiers = modifiers or (1 shl 0)
        if (metaState and KeyEvent.META_SHIFT_ON != 0) modifiers = modifiers or (1 shl 1)
        if (metaState and KeyEvent.META_ALT_LEFT_ON != 0) modifiers = modifiers or (1 shl 2)
        if (metaState and KeyEvent.META_ALT_RIGHT_ON != 0) modifiers = modifiers or (1 shl 3)
        if (metaState and KeyEvent.META_META_ON != 0) modifiers = modifiers or (1 shl 4)
        if (metaState and KeyEvent.META_CAPS_LOCK_ON != 0) modifiers = modifiers or (1 shl 8)
        if (metaState and KeyEvent.META_SCROLL_LOCK_ON != 0) modifiers = modifiers or (1 shl 9)
        if (metaState and KeyEvent.META_NUM_LOCK_ON != 0) modifiers = modifiers or (1 shl 10)
        return modifiers
    }

    private fun androidKeyCodeToHidUsage(keyCode : Int) : Int? = when (keyCode) {
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 0x04 + keyCode - KeyEvent.KEYCODE_A
        in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9 -> 0x1E + keyCode - KeyEvent.KEYCODE_1
        KeyEvent.KEYCODE_0 -> 0x27
        KeyEvent.KEYCODE_ENTER -> 0x28
        KeyEvent.KEYCODE_ESCAPE -> 0x29
        KeyEvent.KEYCODE_DEL -> 0x2A
        KeyEvent.KEYCODE_TAB -> 0x2B
        KeyEvent.KEYCODE_SPACE -> 0x2C
        KeyEvent.KEYCODE_MINUS -> 0x2D
        KeyEvent.KEYCODE_EQUALS -> 0x2E
        KeyEvent.KEYCODE_LEFT_BRACKET -> 0x2F
        KeyEvent.KEYCODE_RIGHT_BRACKET -> 0x30
        KeyEvent.KEYCODE_BACKSLASH -> 0x31
        KeyEvent.KEYCODE_SEMICOLON -> 0x33
        KeyEvent.KEYCODE_APOSTROPHE -> 0x34
        KeyEvent.KEYCODE_GRAVE -> 0x35
        KeyEvent.KEYCODE_COMMA -> 0x36
        KeyEvent.KEYCODE_PERIOD -> 0x37
        KeyEvent.KEYCODE_SLASH -> 0x38
        KeyEvent.KEYCODE_CAPS_LOCK -> 0x39
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 0x3A + keyCode - KeyEvent.KEYCODE_F1
        KeyEvent.KEYCODE_SYSRQ -> 0x46
        KeyEvent.KEYCODE_SCROLL_LOCK -> 0x47
        KeyEvent.KEYCODE_BREAK -> 0x48
        KeyEvent.KEYCODE_INSERT -> 0x49
        KeyEvent.KEYCODE_MOVE_HOME -> 0x4A
        KeyEvent.KEYCODE_PAGE_UP -> 0x4B
        KeyEvent.KEYCODE_FORWARD_DEL -> 0x4C
        KeyEvent.KEYCODE_MOVE_END -> 0x4D
        KeyEvent.KEYCODE_PAGE_DOWN -> 0x4E
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x4F
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x50
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x51
        KeyEvent.KEYCODE_DPAD_UP -> 0x52
        KeyEvent.KEYCODE_NUM_LOCK -> 0x53
        KeyEvent.KEYCODE_NUMPAD_DIVIDE -> 0x54
        KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> 0x55
        KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> 0x56
        KeyEvent.KEYCODE_NUMPAD_ADD -> 0x57
        KeyEvent.KEYCODE_NUMPAD_ENTER -> 0x58
        in KeyEvent.KEYCODE_NUMPAD_1..KeyEvent.KEYCODE_NUMPAD_9 -> 0x59 + keyCode - KeyEvent.KEYCODE_NUMPAD_1
        KeyEvent.KEYCODE_NUMPAD_0 -> 0x62
        KeyEvent.KEYCODE_NUMPAD_DOT -> 0x63
        KeyEvent.KEYCODE_MENU -> 0x65
        KeyEvent.KEYCODE_CTRL_LEFT -> 0xE0
        KeyEvent.KEYCODE_SHIFT_LEFT -> 0xE1
        KeyEvent.KEYCODE_ALT_LEFT -> 0xE2
        KeyEvent.KEYCODE_META_LEFT -> 0xE3
        KeyEvent.KEYCODE_CTRL_RIGHT -> 0xE4
        KeyEvent.KEYCODE_SHIFT_RIGHT -> 0xE5
        KeyEvent.KEYCODE_ALT_RIGHT -> 0xE6
        KeyEvent.KEYCODE_META_RIGHT -> 0xE7
        else -> null
    }

    /**
     * The last value of the axes so the stagnant axes can be eliminated to not wastefully look them up
     */
    private val axesHistory = FloatArray(MotionHostEvent.axes.size)
    private val mousePositionHistory = mutableMapOf<Int, Pair<Int, Int>>()

    /**
     * Handles translating any [MotionHostEvent]s to a [GuestEvent] that is passed into libskyline
     */
    fun handleMotionEvent(event : MotionEvent) : Boolean {
        if ((event.isFromSource(InputDevice.SOURCE_CLASS_JOYSTICK) || event.isFromSource(InputDevice.SOURCE_CLASS_BUTTON)) && event.action == MotionEvent.ACTION_MOVE) {
            for (axisItem in MotionHostEvent.axes.withIndex()) {
                val axis = axisItem.value
                val range : InputDevice.MotionRange? = event.device.getMotionRange(axis, event.source)
                var value = event.getAxisValue(axis)
                range?.let {
                    val flat = maxOf(it.flat, MIN_AXIS_DEAD_ZONE)
                    value = if (abs(value) > flat)
                        if (value > 0)
                            (value - flat) / (it.max - flat)
                        else
                            -((abs(value) - flat) / (abs(it.min) - flat))
                    else
                        0f
                } ?: run {
                    value = if (abs(value) > MIN_AXIS_DEAD_ZONE) value else 0f
                }

                if ((event.historySize != 0 && value != event.getHistoricalAxisValue(axis, 0)) || axesHistory[axisItem.index] != value) {
                    var polarity = value > 0 || (value == 0f && axesHistory[axisItem.index] >= 0)

                    val guestEvent = MotionHostEvent(event.device.descriptor, axis, polarity).let { hostEvent ->
                        inputManager.eventMap[hostEvent] ?: if (value == 0f) {
                            polarity = false
                            inputManager.eventMap[hostEvent.copy(polarity = false)]
                        } else {
                            null
                        }
                    }

                    when (guestEvent) {
                        is ButtonGuestEvent -> {
                            val action = if (abs(value) >= guestEvent.threshold) ButtonState.Pressed.state else ButtonState.Released.state
                            if (guestEvent.button != ButtonId.Menu)
                                setButtonState(guestEvent.id, guestEvent.button.value, action)
                        }

                        is AxisGuestEvent -> {
                            value = guestEvent.value(value)
                            value = if (polarity) abs(value) else -abs(value)
                            value = if (guestEvent.axis == AxisId.LX || guestEvent.axis == AxisId.RX) value else -value
                            val axisValue = (value * Short.MAX_VALUE).toInt()
                            traceAxisSource(2, guestEvent.id, guestEvent.axis.ordinal, axisValue)
                            setAxisValue(guestEvent.id, guestEvent.axis.ordinal, axisValue)
                        }
                    }
                }

                axesHistory[axisItem.index] = value
            }

            return true
        }

        if (event.isFromSource(InputDevice.SOURCE_MOUSE) || event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)) {
            when (event.actionMasked) {
                MotionEvent.ACTION_MOVE,
                MotionEvent.ACTION_HOVER_MOVE,
                MotionEvent.ACTION_SCROLL,
                MotionEvent.ACTION_BUTTON_PRESS,
                MotionEvent.ACTION_BUTTON_RELEASE -> {
                    val x = event.x.toInt()
                    val y = event.y.toInt()
                    val previous = mousePositionHistory.put(event.deviceId, Pair(x, y))
                    val relativeX = event.getAxisValue(MotionEvent.AXIS_RELATIVE_X).toInt()
                    val relativeY = event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y).toInt()
                    val deltaX = if (relativeX != 0) relativeX else previous?.let { x - it.first } ?: 0
                    val deltaY = if (relativeY != 0) relativeY else previous?.let { y - it.second } ?: 0

                    var buttons = 0
                    if (event.buttonState and MotionEvent.BUTTON_PRIMARY != 0) buttons = buttons or (1 shl 0)
                    if (event.buttonState and MotionEvent.BUTTON_SECONDARY != 0) buttons = buttons or (1 shl 1)
                    if (event.buttonState and MotionEvent.BUTTON_TERTIARY != 0) buttons = buttons or (1 shl 2)
                    if (event.buttonState and MotionEvent.BUTTON_FORWARD != 0) buttons = buttons or (1 shl 3)
                    if (event.buttonState and MotionEvent.BUTTON_BACK != 0) buttons = buttons or (1 shl 4)

                    setMouseState(
                        x,
                        y,
                        deltaX,
                        deltaY,
                        event.getAxisValue(MotionEvent.AXIS_HSCROLL).toInt(),
                        event.getAxisValue(MotionEvent.AXIS_VSCROLL).toInt(),
                        buttons
                    )
                    return true
                }
            }
        }

        return false
    }

    override fun onAccuracyChanged(sensor : Sensor?, accuracy : Int) {}

    /**
     * This handles translating any [SensorEvent]s to a [GuestEvent] that is passed into libskyline
     */
    override fun onSensorChanged(event : SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                motionSensor.accelerometer[0] = motionAcelOrientation[0] * event.values[1] / SensorManager.GRAVITY_EARTH
                motionSensor.accelerometer[1] = motionAcelOrientation[1] * event.values[0] / SensorManager.GRAVITY_EARTH
                motionSensor.accelerometer[2] = motionAcelOrientation[2] * event.values[2] / SensorManager.GRAVITY_EARTH
            }

            Sensor.TYPE_GYROSCOPE -> {
                // Investigate why sensor value is off by 12x
                motionSensor.gyroscope[0] = motionGyroOrientation[0] * event.values[1] / 12.0f
                motionSensor.gyroscope[1] = motionGyroOrientation[1] * event.values[0] / 12.0f
                motionSensor.gyroscope[2] = motionGyroOrientation[2] * event.values[2] / 12.0f
            }

            Sensor.TYPE_ROTATION_VECTOR -> {
                motionSensor.quaternion[0] = event.values[1]
                motionSensor.quaternion[1] = event.values[0]
                motionSensor.quaternion[2] = event.values[2]
                motionSensor.quaternion[3] = event.values[3]
                SensorManager.getRotationMatrixFromVector(motionRotationMatrix, motionSensor.quaternion)
                SensorManager.remapCoordinateSystem(motionRotationMatrix, motionAxisOrientationX, motionAxisOrientationY, motionSensor.orientationMatrix)
            }

            Sensor.TYPE_GAME_ROTATION_VECTOR -> {
                motionSensor.quaternion[0] = event.values[1]
                motionSensor.quaternion[1] = event.values[0]
                motionSensor.quaternion[2] = event.values[2]
                motionSensor.quaternion[3] = event.values[3]
                SensorManager.getRotationMatrixFromVector(motionRotationMatrix, motionSensor.quaternion)
                SensorManager.remapCoordinateSystem(motionRotationMatrix, motionAxisOrientationX, motionAxisOrientationY, motionSensor.orientationMatrix)
            }

            else -> {}
        }

        // Only update state on accelerometer data
        if (event.sensor.type != Sensor.TYPE_ACCELEROMETER)
            return

        motionSensor.deltaTimestamp = event.timestamp.toULong() - motionSensor.timestamp
        motionSensor.timestamp = event.timestamp.toULong()
        motionDataBuffer.clear()
        setMotionState(0, 0, motionSensor.writeToByteBuffer(motionDataBuffer))
        motionDataBuffer.clear()
        setMotionState(0, 1, motionSensor.writeToByteBuffer(motionDataBuffer))
        motionDataBuffer.clear()
        setMotionState(0, 2, motionSensor.writeToByteBuffer(motionDataBuffer))
    }

    fun handleTouchEvent(view : View, event : MotionEvent) : Boolean {
        if (view.width <= 0 || view.height <= 0)
            return false

        val count = event.pointerCount
        val points = IntArray(count * 7) // This is an array of skyline::input::TouchScreenPoint in C++ as that allows for efficient transfer of values to it
        var offset = 0
        for (index in 0 until count) {
            val pointer = MotionEvent.PointerCoords()
            event.getPointerCoords(index, pointer)

            val x = (pointer.x * 1280 / view.width).coerceIn(0f, 1279f).toInt()
            val y = (pointer.y * 720 / view.height).coerceIn(0f, 719f).toInt()

            val attribute = when {
                event.actionMasked == MotionEvent.ACTION_CANCEL -> 2
                index != event.actionIndex -> 0
                event.actionMasked == MotionEvent.ACTION_DOWN || event.actionMasked == MotionEvent.ACTION_POINTER_DOWN -> 1
                event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_POINTER_UP -> 2
                else -> 0
            }

            points[offset++] = attribute
            points[offset++] = event.getPointerId(index)
            points[offset++] = x
            points[offset++] = y
            points[offset++] = pointer.touchMinor.toInt()
            points[offset++] = pointer.touchMajor.toInt()
            points[offset++] = (pointer.orientation * 180 / Math.PI).toInt()
        }

        setTouchState(points)

        return true
    }

    fun getFirstControllerType() : ControllerType {
        return inputManager.controllers[0]?.type ?: ControllerType.None
    }
}
