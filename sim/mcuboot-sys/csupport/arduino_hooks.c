/*
 * Builds the Arduino MCUboot image access hooks (ARDUINO_MCUBOOT_HOOKS_FILE)
 * with the simulator's warning flags: the default hooks ignore their
 * parameters.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ARDUINO_MCUBOOT_HOOKS_FILE
