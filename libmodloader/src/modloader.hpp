#pragma once

#include <stdlib.h>

// ModContext
class ModContext
{
    public:
        ModContext(std::string path_, void* handle_) : path(path_), handle(handle_) {}
        std::string path;
        void* handle;
};