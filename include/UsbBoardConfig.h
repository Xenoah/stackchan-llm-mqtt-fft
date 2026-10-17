#pragma once
// CoreS3's variant defines USB_PID unconditionally. Load it first, then select
// a separate prototype identity so Windows does not reuse USB-JTAG bindings.
#ifndef __ASSEMBLER__
#include "pins_arduino.h"
#undef USB_PID
#define USB_PID 0x8001
#endif
