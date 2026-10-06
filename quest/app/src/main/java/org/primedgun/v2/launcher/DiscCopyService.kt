// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import kotlin.concurrent.thread
import org.primedgun.v2.R

/**
 * Runs [DiscCopy] as a foreground service, as Wiicompiled VR's GameSetupService does
 * its disc extraction: copying a 1.4 GB image takes a while, a panel app's process is
 * otherwise fair game once the panel is closed, and taking the headset off sleeps the
 * CPU, hence the partial wake lock.
 */
class DiscCopyService : Service() {

    private var lastNotified = 0L
    private val listener: (DiscCopy.State) -> Unit = { state ->
        val now = SystemClock.elapsedRealtime()
        if (state !is DiscCopy.State.Copying || now - lastNotified >= NOTIFY_INTERVAL_MS) {
            lastNotified = now
            getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, notification(state))
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService obliges the service to go foreground even when it has
        // nothing to do, or Android ends the app.
        createChannel()
        startForeground(NOTIFICATION_ID, notification(DiscCopy.state), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        val uri = intent?.data
        if (uri == null || DiscCopy.isRunning) {
            if (!DiscCopy.isRunning) {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
            }
            return START_NOT_STICKY
        }
        DiscCopy.addListener(listener)
        val wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PrimedGun:DiscCopy")
        wakeLock.acquire(WAKE_LOCK_TIMEOUT_MS)
        thread(name = "DiscCopy") {
            try {
                DiscCopy.run(applicationContext, uri)
            } finally {
                if (wakeLock.isHeld) wakeLock.release()
                mainExecutor.execute {
                    DiscCopy.removeListener(listener)
                    stopForeground(STOP_FOREGROUND_REMOVE)
                    stopSelf()
                }
            }
        }
        return START_NOT_STICKY
    }

    private fun createChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.primedgun_disc_copy_channel), NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(state: DiscCopy.State): Notification {
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.primedgun_logo)
            .setContentTitle(getString(R.string.primedgun_disc_copy_title))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        if (state is DiscCopy.State.Copying && state.total > 0) {
            builder.setContentText(getString(R.string.primedgun_disc_copying, state.percent))
                .setProgress(100, state.percent, false)
        } else {
            builder.setProgress(0, 0, true)
        }
        return builder.build()
    }

    companion object {
        private const val CHANNEL_ID = "disc_copy"
        private const val NOTIFICATION_ID = 1
        private const val NOTIFY_INTERVAL_MS = 1000L
        // Longer than any real copy; only a hung one would reach it.
        private const val WAKE_LOCK_TIMEOUT_MS = 30L * 60 * 1000

        fun start(context: Context, uri: Uri) {
            val intent = Intent(context, DiscCopyService::class.java)
                .setData(uri)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            context.startForegroundService(intent)
        }
    }
}
