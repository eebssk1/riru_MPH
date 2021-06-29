# Riru - MPH

Hook system prop function with dobby to simulate miui for MiPushFOSS

//Code borrowed from many other projects and squashed to one file.
Newer Android first check if the prop actually exist in JNI,so directly hook the get method will not work.
We create a fake prop object so we no longer need to hook the get method.

STRICTLY AOSP BASED -> https://android.googlesource.com/platform/frameworks/base/+/refs/heads/master/core/jni/android_os_SystemProperties.cpp#70

TODO: make this project look more serious.
