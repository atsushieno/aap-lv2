plugins {
    alias(libs.plugins.android.application)
}

val enable_asan: Boolean by extra

android {
    namespace = "org.androidaudioplugin.aap_ayumi"
    compileSdk = libs.versions.android.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "org.androidaudioplugin.aap_ayumi"
        minSdk = libs.versions.android.minSdk.get().toInt()
        targetSdk = libs.versions.android.targetSdk.get().toInt()
        versionCode = 1
        versionName = libs.versions.aap.lv2.get()

        externalNativeBuild {
            cmake {
                // https://github.com/google/prefab/blob/bccf5a6a75b67add30afbb6d4f7a7c50081d2d86/api/src/main/kotlin/com/google/prefab/api/Android.kt#L243
                arguments ("-DANDROID_STL=c++_shared", "-DAAP_ENABLE_ASAN=" + (if (enable_asan) "1" else "0"))
            }
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    ndkVersion = libs.versions.ndk.get()

    buildTypes {
        debug {
            packaging.jniLibs.keepDebugSymbols.add("**/*.so")
        }
        release {
            isMinifyEnabled = false
            proguardFiles (getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            version = libs.versions.cmake.get()
            path ("src/main/cpp/CMakeLists.txt")
        }
    }

    if (enable_asan)
        packaging.jniLibs.useLegacyPackaging = true

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation (project(":androidaudioplugin-lv2"))
    implementation (libs.aap.core)
    implementation (libs.aap.ui.compose.app)
    implementation (libs.aap.ui.web)
    implementation (libs.aap.midi.device.service)
    androidTestImplementation (libs.aap.testing)
    //  If you want to test aap-core locally, switch to these local references
    //  (along with settings.gradle.kts changes)
    /*
    implementation (project (":androidaudioplugin"))
    implementation (project (":androidaudioplugin-ui-compose-app"))
    implementation (project (":androidaudioplugin-ui-web"))
    implementation (project (":androidaudioplugin-midi-device-service"))
    androidTestImplementation (project (":androidaudioplugin-testing"))
     */

    testImplementation (libs.junit)
    androidTestImplementation (libs.test.ext.junit)
    androidTestImplementation (libs.test.espresso.core)
}

// Starting AGP 7.0.0-alpha05, AGP stopped caring build dependencies and it broke builds.
// This is a forcible workarounds to build libandroidaudioplugin.so in prior to referencing it.
gradle.projectsEvaluated {
    tasks["buildCMakeDebug"].dependsOn(rootProject.project("androidaudioplugin-lv2").tasks["mergeDebugNativeLibs"])
    tasks["buildCMakeRelWithDebInfo"].dependsOn(rootProject.project("androidaudioplugin-lv2").tasks["mergeReleaseNativeLibs"])
}
