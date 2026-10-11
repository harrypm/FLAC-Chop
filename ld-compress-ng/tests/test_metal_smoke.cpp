#include "metal_devices.h"

#include <iostream>

int main()
{
    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal smoke skipped: Metal support was not built\n";
        return 0;
    }
    std::cout << "Metal smoke skipped: Metal Objective-C++ test was not built\n";
    return 0;
}
