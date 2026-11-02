#pragma once
// On-device hardware self-test. Camera and flash storage must already be initialised.
// Returns the number of failed checks (0 = all good).
int selftest_run(void);
