#pragma once

#include <android/log.h>

#define log(priority, message) __android_log_print(priority, "libmain - patched", message)
#define logfp(priority, ...) __android_log_print(priority, "libmain - patched", __VA_ARGS__)