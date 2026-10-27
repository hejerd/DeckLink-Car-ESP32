plugins { id("com.android.application") }

android {
    namespace = "com.hejerd.decklink"
    compileSdk = 33
    defaultConfig {
        applicationId = "com.hejerd.decklink"
        minSdk = 24
        targetSdk = 33
        versionCode = 1
        versionName = "1.0.0"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
dependencies {
    implementation("androidx.core:core-ktx:1.10.1")
    implementation("androidx.appcompat:appcompat:1.6.1")
}
