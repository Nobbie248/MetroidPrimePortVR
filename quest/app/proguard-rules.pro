# Native code calls into these by name (SDL's JNI, Aurora's surface, the game's
# requestQuit); keep them if minification is ever turned on.
-keep class org.libsdl.app.** { *; }
-keep class dev.encounter.aurora.** { *; }
-keep class org.primedgun.v2.** { *; }
