#pragma once

#include <android/log.h>

#define logp(priority, message) __android_log_print(priority, "libmodloader", message)
#define logpf(priority, ...) __android_log_print(priority, "libmodloader", __VA_ARGS__)
#define logpfm(priority, ...) __android_log_print(priority, info.tag.c_str(), __VA_ARGS__)