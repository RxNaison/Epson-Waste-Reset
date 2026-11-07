# The native library calls back into these by name.
-keep class io.github.rxnaison.ewr.EwrNative { *; }
-keep interface io.github.rxnaison.ewr.EwrCallbacks { *; }
-keep class * implements io.github.rxnaison.ewr.EwrCallbacks { *; }
