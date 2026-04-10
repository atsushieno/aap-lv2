plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.compose.compiler) apply false
    alias(libs.plugins.dokka) apply false
    alias(libs.plugins.vanniktech.maven.publish) apply false
}

apply { from ("${rootDir}/publish-root.gradle") }

subprojects {
    val enable_asan: Boolean by extra(false)

    group = "org.androidaudioplugin"
    repositories {
        google()
        mavenLocal()
        mavenCentral()
        maven ("https://plugins.gradle.org/m2/")
        maven ("https://jitpack.io")
        maven ("https://maven.pkg.jetbrains.space/public/p/compose/dev")
    }
}

tasks.register<Delete>("clean") {
    delete(rootProject.buildDir)
}
