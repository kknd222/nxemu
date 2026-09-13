package org.nxemu.app

import android.app.Application

class NxEmuApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        NativeLibrary.setApplicationContext(this)
    }
}
