import java.io.File

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// One version for every build of EWR: project(EWR VERSION ...) in the root
// CMakeLists.txt, which the release workflow checks the tag against.
val ewrVersion: String = run {
    val cmake = rootProject.file("../CMakeLists.txt").readText()
    Regex("""project\(EWR VERSION ([0-9]+\.[0-9]+\.[0-9]+)""").find(cmake)?.groupValues?.get(1)
        ?: error("No project(EWR VERSION ...) in CMakeLists.txt")
}
val ewrVersionCode: Int = ewrVersion.split(".").map { it.toInt() }.let { (major, minor, patch) ->
    major * 10000 + minor * 100 + patch
}

// The release key comes from the environment (CI secrets); without one the
// release build is left unsigned, and a local build uses debug.
val releaseKeystore: String? = System.getenv("EWR_KEYSTORE")

android {
    namespace = "io.github.rxnaison.ewr"
    compileSdk = 37
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "io.github.rxnaison.ewr"
        minSdk = 26
        targetSdk = 36
        versionCode = ewrVersionCode
        versionName = ewrVersion

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = File(releaseKeystore)
                storePassword = System.getenv("EWR_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("EWR_KEY_ALIAS")
                keyPassword = System.getenv("EWR_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (releaseKeystore != null)
                signingConfig = signingConfigs.getByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }
}

kotlin {
    jvmToolchain(21)
}

// database.json ships inside the APK, taken from the repository root at build
// time so the app never carries a copy of its own to drift.
abstract class CopyDatabase : DefaultTask() {
    @get:InputFile
    abstract val database: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val out = outputDir.get().asFile
        out.mkdirs()
        database.get().asFile.copyTo(File(out, "database.json"), overwrite = true)
    }
}

val copyDatabase = tasks.register<CopyDatabase>("copyDatabase") {
    database.set(rootProject.layout.projectDirectory.file("../database.json"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(copyDatabase, CopyDatabase::outputDir)
        variant.outputs.forEach { output ->
            @Suppress("UnstableApiUsage")
            output.outputFileName.set("EWR-v${ewrVersion}-android.apk")
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
}
