plugins {
    id("com.android.application")
}

// AirLink on Android. The native code (app/CMakeLists.txt: Kompoz, the UI and the Rust core, built as
// libmain.so), Kompoz's Java side and its bundled fonts come from outside this folder.
val appDir: File = rootProject.file("..")                       // AirLink/app
// The Kompoz checkout next to AirLink, or another one (-PkompozDir=/path, e.g. a branch's worktree).
val kompozRoot: File = ((findProperty("kompozDir") as String?)?.let(::File) ?: File(appDir, "../../kompoz")).canonicalFile

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
        // VERSION at the repository's root, for every platform; the code grows with it (0.2.0 -> 200).
        val version = rootProject.file("../../VERSION").readText().trim()
        val (major, minor, patch) = version.split(".").map { it.takeWhile(Char::isDigit).toInt() }
        versionCode = major * 10000 + minor * 100 + patch
        versionName = version
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

    // Release signing from the environment (CI secrets, or a local keystore): AIRLINK_KEYSTORE (the
    // .jks file), AIRLINK_KEYSTORE_PASSWORD, AIRLINK_KEY_ALIAS, AIRLINK_KEY_PASSWORD. Without them a
    // release build is unsigned.
    val keystore = System.getenv("AIRLINK_KEYSTORE")?.let(::File)?.takeIf { it.isFile }
    signingConfigs {
        if (keystore != null) create("release") {
            storeFile = keystore
            storePassword = System.getenv("AIRLINK_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("AIRLINK_KEY_ALIAS")
            keyPassword = System.getenv("AIRLINK_KEY_PASSWORD") ?: System.getenv("AIRLINK_KEYSTORE_PASSWORD")
        }
    }
    buildTypes {
        getByName("release") {
            // The native side reaches Kotlin classes and methods by name (JNI): no shrinking.
            isMinifyEnabled = false
            if (keystore != null) signingConfig = signingConfigs.getByName("release")
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
