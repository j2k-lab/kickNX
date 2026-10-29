#pragma once

namespace kicknx::haptics {

float keyboardIntensity();
bool setKeyboardIntensity(float intensity);

void keyboardPulse();
void stop();

}  // namespace kicknx::haptics
