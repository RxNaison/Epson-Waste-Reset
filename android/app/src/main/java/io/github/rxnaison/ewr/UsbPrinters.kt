package io.github.rxnaison.ewr

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.core.content.ContextCompat

/**
 * Epson printers on USB (an OTG cable on a phone). An app may not enumerate
 * USB itself: it asks the user, opens the device through UsbManager, and
 * hands EWR the file descriptor, which EWR wraps and never closes.
 */
class UsbPrinters(private val context: Context) {

    private val manager = context.getSystemService(UsbManager::class.java)
    private val connections = mutableMapOf<String, UsbDeviceConnection>()

    fun epsonDevices(): List<UsbDevice> =
        manager.deviceList.values.filter { it.vendorId == EPSON_VENDOR_ID }

    fun hasPermission(device: UsbDevice): Boolean = manager.hasPermission(device)

    /** Asks the user once; [onResult] runs on the main thread. */
    fun requestPermission(device: UsbDevice, onResult: (Boolean) -> Unit) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                ctx.unregisterReceiver(this)
                onResult(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
            }
        }
        ContextCompat.registerReceiver(
            context, receiver, IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED,
        )

        // Mutable: the system fills in the device and the answer.
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
        manager.requestPermission(device, PendingIntent.getBroadcast(context, 0, intent, flags))
    }

    /**
     * Opens every permitted Epson device and hands EWR their descriptors.
     * Each interface is claimed with force once - that takes it from any
     * kernel driver, which an app may not detach any other way - and
     * released again for EWR to claim on the same descriptor.
     */
    fun connect(): Int {
        for (device in epsonDevices()) {
            if (!hasPermission(device) || connections.containsKey(device.deviceName))
                continue

            val connection = manager.openDevice(device) ?: continue
            for (i in 0 until device.interfaceCount) {
                val usbInterface = device.getInterface(i)
                if (connection.claimInterface(usbInterface, true))
                    connection.releaseInterface(usbInterface)
            }
            connections[device.deviceName] = connection
        }

        // A device unplugged since stays out.
        val present = epsonDevices().map { it.deviceName }.toSet()
        connections.keys.filter { it !in present }.forEach { name -> connections.remove(name)?.close() }

        EwrNative.setUsbFds(connections.values.map { it.fileDescriptor }.toIntArray())
        return connections.size
    }

    fun disconnect() {
        EwrNative.setUsbFds(IntArray(0))
        connections.values.forEach { it.close() }
        connections.clear()
    }

    companion object {
        const val EPSON_VENDOR_ID = 0x04B8
        private const val ACTION_USB_PERMISSION = "io.github.rxnaison.ewr.USB_PERMISSION"
    }
}
