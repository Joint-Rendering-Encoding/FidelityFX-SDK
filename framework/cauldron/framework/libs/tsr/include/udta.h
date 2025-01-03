#pragma once

#include <windows.h>
#include <string>

void tsr_map_udta(const std::string& udtaSuffix,        // UDTA suffix
                  size_t             structSize,        // Size of the struct (in bytes)
                  size_t             bufferCount,       // Number of buffers
                  HANDLE&            sharedDataHandle,  // Shared memory handle (output)
                  LPVOID&            sharedView,        // Shared memory view (output)
                  void**             sharedDataArray    // Array of pointers to shared data (output)
);

void tsr_unmap_udta(LPVOID& sharedView,        // Shared memory view (input/output)
                    HANDLE& sharedDataHandle,  // Shared memory handle (input/output)
                    void**  sharedDataArray,   // Array of pointers to shared data (input/output)
                    size_t  bufferCount        // Number of buffers
);
