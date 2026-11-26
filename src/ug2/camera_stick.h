#ifndef UG2_CAMERA_STICK_H_
#define UG2_CAMERA_STICK_H_

#include <x86.h>

// The fork's right stick camera (camera_stick, src/ug2/camera_stick.cpp), on in place of 0.9.0's free look.
namespace ug2
{

// From the chase camera update's native (ecx the mover): false when camera_stick is off (free look's turn).
bool cameraStickOn(x86::CPU& cpu);
// After the game's update.
void cameraStickEnd();
// From the look-at's native, before the game's look-at.
void cameraStickLookAt(x86::CPU& cpu);

}

#endif
