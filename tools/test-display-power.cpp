#include "../DisplayPower.h"
#include <stdio.h>

// Standalone memory-only tests: never links or calls the app, Win32, WMI or DDC/CI.
int main() {
    int count = 0;
    bool ok = displaypower::Checks(&count);
    printf("Display-power decision tests: %s (%d checks)\n", ok ? "PASS" : "FAIL", count);
    return ok ? 0 : 1;
}
