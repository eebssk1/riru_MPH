# Riru - MPH

Hook system prop function with dobby to simulate miui for MiPushFOSS

//Code borrowed from many other projects and squashed to one file.
Newer Android first check if the prop actually exist in JNI,so directly hook the get method will not work.
We create a fake prop object so we no longer need to hook the get method.

TODO: make this project look more serious.
