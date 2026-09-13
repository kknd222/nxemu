package org.nxemu.input

class NxemuInputDevice(
    private val name: String = "Android Input",
    private val guid: String = "00000000-0000-0000-0000-000000000000",
    private val port: Int = 0,
    private val supportsVibration: Boolean = false,
    private val axes: Array<Int> = emptyArray()
) {
    fun getName(): String = name
    fun getGUID(): String = guid
    fun getPort(): Int = port
    fun getSupportsVibration(): Boolean = supportsVibration
    fun getAxes(): Array<Int> = axes
    fun hasKeys(keys: IntArray): BooleanArray = BooleanArray(keys.size) { false }
    fun vibrate(intensity: Float) {
        @Suppress("UNUSED_EXPRESSION")
        intensity
    }
}
