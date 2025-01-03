#pragma once
#include <exception>

#define AssertCritical(condition, message)                                       \
    do                                                                           \
    {                                                                            \
        if (!(condition))                                                        \
        {                                                                        \
            MessageBoxW(NULL, message, L"Critical Error", MB_OK | MB_ICONERROR); \
            std::terminate();                                                    \
        }                                                                        \
    } while (0)
