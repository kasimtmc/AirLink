plugins {
    id("com.android.application")
}

// AirLink on Android. The native code (app/CMakeLists.txt: Kompoz, the UI and the Rust core, built as
// libmain.so), Kompoz's Java side and its bundled fonts come from outside this folder.
val appDir: File = rootProject.file("..")                       // AirLink/app
val kompozRoot: File = File(appDir, "../../kompoz").canonicalFile  // the Kompoz checkout next to AirLink

android {
    namespace = "com.plyg.airlink"
    compileSdk {
        version = release(36)
    }
    ndkVersion = "29.0.14206865"

    defaultConfig {
        // Android application identity. Changing this from dev.airlink.airlink creates a separate
        // identity; users with no surviving old app data need to pair again.
        applicationId = "com.plyg.airlink"
        minSdk = 24
        targetSdk = 36
        versionCode = 2
        versionName = "0.2.0"
        ndk {
            // Keep the phone APK small; CI/emulator builds can request another ABI explicitly.
            abiFilters += (providers.gradleProperty("airlinkAbi").orNull ?: "arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DKOMPOZ_DIR=${kompozRoot.absolutePath}",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(appDir, "CMakeLists.txt")
            version = "4.1.2"
        }
    }

    sourceSets["main"].java.srcDir(File(kompozRoot, "platform/android/java"))
    sourceSets["main"].assets.srcDir(File(kompozRoot, "assets"))

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.activity:activity:1.13.0")
    implementation("androidx.camera:camera-core:1.7.0")
    implementation("androidx.camera:camera-camera2:1.7.0")
    implementation("androidx.camera:camera-lifecycle:1.7.0")
    implementation("androidx.camera:camera-view:1.7.0")
    implementation("com.google.mediapipe:tasks-vision:1.2.3")
}
