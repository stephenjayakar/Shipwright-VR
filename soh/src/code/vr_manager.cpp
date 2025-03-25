#include "vr_manager.h"
#include <iostream>

extern "C" {

int halt() {
    std::cout << "Press Enter to continue..." << std::endl;
    std::cin.get(); // blocks until user presses Enter
    return 0;
}

} // extern "C"