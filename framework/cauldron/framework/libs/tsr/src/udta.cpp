#include "tsr.h"
#include "assert.h"

#include <sstream>
#include <locale>
#include <codecvt>

void tsr_map_udta(const std::string& udtaSuffix, size_t structSize, size_t bufferCount, HANDLE& sharedDataHandle, LPVOID& sharedView, void** sharedDataArray)
{
    // Only map shared data if we haven't already
    if (sharedDataArray[0] != nullptr)
        return;

    // Create the shared data name
    std::stringstream sharedDataName;
    sharedDataName << "Local\\" << udtaSuffix;

    // Create or open the shared memory file mapping
    sharedDataHandle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, structSize * bufferCount, sharedDataName.str().c_str());
    AssertCritical(sharedDataHandle, L"Failed to create shared data file mapping");

    // Map the shared memory file mapping into our address space
    sharedView = MapViewOfFile(sharedDataHandle, FILE_MAP_ALL_ACCESS, 0, 0, structSize * bufferCount);
    AssertCritical(sharedView, L"Failed to map shared data file mapping");

    // Initialize shared data, if not already done
    for (size_t i = 0; i < bufferCount; i++)
    {
        if (sharedDataArray[i] == nullptr)
        {
            sharedDataArray[i] = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(sharedView) + (structSize * i));
        }
    }
}

void tsr_unmap_udta(LPVOID& sharedView, HANDLE& sharedDataHandle, void** sharedDataArray, size_t bufferCount)
{
    // Unmap the shared memory view if it's already mapped
    if (sharedDataArray[0])
    {
        UnmapViewOfFile(sharedView);
        sharedView = nullptr;

        for (size_t i = 0; i < bufferCount; i++)
        {
            sharedDataArray[i] = nullptr;
        }
    }

    // Close the shared memory handle if it's open
    if (sharedDataHandle)
    {
        CloseHandle(sharedDataHandle);
        sharedDataHandle = nullptr;
    }
}
