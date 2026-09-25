plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.dokka)
    alias(libs.plugins.vanniktech.maven.publish)
    signing
}

apply { from ("../publish-pom.gradle") }

version = libs.versions.aap.lv2.get()

val enable_asan: Boolean by extra

android {
    namespace = "org.androidaudioplugin.lv2"
    compileSdk = libs.versions.android.compileSdk.get().toInt()

    defaultConfig {
        minSdk = libs.versions.android.minSdk.get().toInt()

        externalNativeBuild {
            cmake {
                // https://github.com/google/prefab/blob/bccf5a6a75b67add30afbb6d4f7a7c50081d2d86/api/src/main/kotlin/com/google/prefab/api/Android.kt#L243
                arguments ("-DANDROID=1", "-DANDROID_STL=c++_shared", "-DBUILD_WITH_PREFAB=1", "-DAAP_ENABLE_ASAN=" + (if (enable_asan) "1" else "0"))
            }
        }
    }
    // See aap-core/androidaudioplugin/build.gradle.kts to find out why it is required...
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

    // https://github.com/google/prefab/issues/127
    packaging.jniLibs.excludes.addAll(listOf("**/libc++_shared.so", "**/libandroidaudioplugin.so"))

    buildFeatures {
        prefab = true
        prefabPublishing = true
    }
    prefab {
        create("androidaudioplugin-lv2") {
            name = "androidaudioplugin-lv2"
        }
    }

    externalNativeBuild {
        cmake {
            version = libs.versions.cmake.get()
            path ("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation (libs.aap.core)
    //  If you want to test aap-core locally, switch to these local references
    //  (along with settings.gradle.kts changes)
    //implementation (project(":androidaudioplugin"))

    implementation (libs.androidx.core.ktx)

    testImplementation (libs.junit)
    androidTestImplementation (libs.test.ext.junit)
    androidTestImplementation (libs.test.espresso.core)
}

val gitProjectName = "aap-lv2"
val packageName = project.name
val packageDescription = "AndroidAudioPlugin - LV2"
// my common settings
val packageUrl = "https://github.com/atsushieno/$gitProjectName"
val licenseName = "the MIT License"
val licenseUrl = "https://github.com/atsushieno/$gitProjectName/blob/main/LICENSE"
val devId = "atsushieno"
val devName = "Atsushi Eno"
val devEmail = "atsushieno@gmail.com"

// Common copy-pasted
mavenPublishing {
    publishToMavenCentral()
    if (project.hasProperty("mavenCentralUsername") || System.getenv("ORG_GRADLE_PROJECT_mavenCentralUsername") != null)
        signAllPublications()
    coordinates(group.toString(), project.name, version.toString())
    pom {
        name.set(packageName)
        description.set(packageDescription)
        url.set(packageUrl)
        scm { url.set(packageUrl) }
        licenses { license { name.set(licenseName); url.set(licenseUrl) } }
        developers { developer { id.set(devId); name.set(devName); email.set(devEmail) } }
    }
}
