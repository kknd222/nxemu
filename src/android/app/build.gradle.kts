import java.io.ByteArrayOutputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val nxemuDir = project(":NxEmu").projectDir
val autoVersion = (((System.currentTimeMillis() / 1000) - 1451606400) / 10).toInt()
val nxemuAbis = (findProperty("nxemuAbi") as String?)
    ?.split(',')
    ?.map { it.trim() }
    ?.filter { it.isNotEmpty() }
    ?.ifEmpty { null }
    ?: listOf("arm64-v8a")

android {
    namespace = "org.nxemu.app"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.nxemu.app"
        minSdk = 24
        targetSdk = 36
        versionCode = autoVersion
        versionName = nxemuGitVersion()

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    "-DBUILD_TESTING=OFF",
                    "-DNXEMU_BUILD_NXEMU_OS=ON",
                    "-DNXEMU_ANDROID_ENABLE_ADRENOTOOLS=ON"
                )
                if (nxemuAbis.any { it == "arm64-v8a" || it == "armeabi-v7a" }) {
                    arguments += "-DANDROID_ARM_NEON=true"
                }
                targets += listOf(
                    "nxemu-android",
                    "nxemu-loader",
                    "nxemu-cpu",
                    "nxemu-video",
                    "nxemu-os"
                )
                if (nxemuAbis.contains("arm64-v8a")) {
                    // adrenotools loads these hook libraries from applicationInfo.nativeLibraryDir
                    // at runtime. Building only the static adrenotools archive is not enough;
                    // without these .so files it silently falls back to the system Vulkan driver.
                    targets += listOf(
                        "hook_impl",
                        "main_hook",
                        "file_redirect_hook",
                        "gsl_alloc_hook"
                    )
                }
                abiFilters += nxemuAbis
            }
        }

        ndk {
            abiFilters += nxemuAbis
        }
    }

    buildFeatures {
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }

    buildTypes {
        debug {
            isDebuggable = true
            // Keep Java/ADB debuggability, but build native code like RelWithDebInfo.
            // A pure CMake Debug build emits mostly "-g" with no -O2/-O3; Dynarmic/GPU
            // hot paths are then far too slow to judge Android gameplay performance.
            isJniDebuggable = false
            versionNameSuffix = "-debug"
            applicationIdSuffix = ".debug"
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG",
                        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG"
                    )
                }
            }
        }
        release {
            isDebuggable = false
            isMinifyEnabled = false
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG",
                        "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG"
                    )
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            version = "3.31.6"
            path = file("${nxemuDir}/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation("androidx.recyclerview:recyclerview:1.4.0")
}

fun nxemuGitVersion(): String {
    val stdout = ByteArrayOutputStream()
    val result = exec {
        workingDir = nxemuDir
        commandLine = listOf("git", "describe", "--tags", "--always", "--dirty")
        standardOutput = stdout
        isIgnoreExitValue = true
    }
    return if (result.exitValue == 0) stdout.toString().trim().ifEmpty { "0.1.0" } else "0.1.0"
}



