#pragma once
// ProsperoLayer PS5 emulator - singleton helper (Kyty-compatible)
#include "common/common.h"
#include <memory>

namespace Common {

template <class T>
class Singleton {
public:
        static T* Instance() {
                static T instance;
                return &instance;
        }

        Singleton(const Singleton&) = delete;
        Singleton& operator=(const Singleton&) = delete;

protected:
        Singleton() = default;
        ~Singleton() = default;
};

} // namespace Common
