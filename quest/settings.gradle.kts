// The Meta Quest build of the port, "PrimedGun v2". It stands beside upstream's
// phone project (android/) rather than inside it: the Quest needs a VR activity,
// its own launcher (Kotlin, AndroidX) and newer build tools, and upstream changes
// android/ with every release.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "PrimedGunQuest"
include(":app")
