#pragma once
#include <sys/mman.h>
#include "log.hpp"
#include <fstream>
#include <sstream>


void protect() {
    // If we look at /proc/self/maps we can see most of what we would probably care about.
    // For each of the things in /proc/self/maps, we probably want to look at the addresses of them and attempt to mprotect with
    // a READ as well as an execute.
    logpf(ANDROID_LOG_VERBOSE, "Protecting memory from /proc/self/maps!");
    std::ifstream procMap("/proc/self/maps");
    std::string line;
    while (std::getline(procMap, line)) {
        logpf(ANDROID_LOG_DEBUG, "%s", line.c_str());
        auto idx = line.find_first_of('-');
        if (idx == std::string::npos) {
            logpf(ANDROID_LOG_ERROR, "Could not find '-' in line: %s", line.c_str());
            continue;
        }
        auto startAddr = std::stoul(line.substr(0, idx), nullptr, 16);
        logpf(ANDROID_LOG_DEBUG, "Start addr: 0x%lx", startAddr);
        auto spaceIdx = line.find_first_of(' ');
        if (spaceIdx == std::string::npos) {
            logpf(ANDROID_LOG_ERROR, "Could not find ' ' in line: %s", line.c_str());
            continue;
        }
        auto endAddr = std::stoul(line.substr(idx + 1, spaceIdx - idx - 1), nullptr, 16);
        logpf(ANDROID_LOG_DEBUG, "End addr: 0x%lx", endAddr);
        // Permissions are 4 characters
        auto perms = line.substr(spaceIdx + 1, 4);
        logpf(ANDROID_LOG_DEBUG, "Perms: %s", perms.c_str());
        if (perms.find('r') == std::string::npos && perms.find('x') != std::string::npos && perms.find('w') != std::string::npos) {
            // If we have execute, and we do not have read, and we do not have write, we need to protect.
            logpf(ANDROID_LOG_INFO, "Protecting memory: 0x%lx - 0x%lx with perms: %s to: +rx", startAddr, endAddr, perms.c_str());
            if (mprotect(reinterpret_cast<void*>(startAddr), endAddr - startAddr, PROT_EXEC | PROT_READ) != 0) {
                logpf(ANDROID_LOG_ERROR, "Protection failed! errno: %s", strerror(errno));
            }
        }
    }
}