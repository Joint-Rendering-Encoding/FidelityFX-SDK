#pragma once

#include "validation_remap.h"
#include <array>
#include <memory>

namespace common
{
    typedef std::array<const cauldron::GPUResource*, 5> FSRResources;
    typedef std::unique_ptr<uint8_t[]>                  FSRData;
}  // namespace common
