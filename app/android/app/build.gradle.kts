plugins {
    id("com.android.application")
}

// AirLink on Android. The native code (app/CMakeLists.txt: Kompoz, the UI and the Rust core, built as
// libmain.so), Kompoz's Java side and its bundled fonts come from outside this folder.
val appDir: File = rootProject.file("..")                       // AirLink/app
val kompozRoot: File = File(appDir, "../../kompoz").canonicalFile  // the Kompoz checkout next to AirLink

android {
    namespace = "dev.airlink.airlink"
    compileSdk {
        version = release(36)
    }
    ndkVersion = "29.0.14206865"

    defaultConfig {
        // The same id as the earlier (Flutter) build: it updates in place and keeps the device's
        // identity and pairings.
        applicationId = "dev.airlink.airlink"
        minSdk = 24
        targetSdk = 36
        versionCode = 2
        versionName = "0.2.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
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
