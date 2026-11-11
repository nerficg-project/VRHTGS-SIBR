#pragma once

namespace sibr
{
    /**
     * Snapshot of the controller inputs, as returned by OpenXRHMD::getControllerState().
     */
    struct ControllerState {
        // Edge-triggered (true only in the frame the button went down), either hand
        bool confirmState = false;  // X (left) / A (right)
        bool backState = false;     // Y (left) / B (right)

        // Thumbstick of whichever hand is deflected further, in [-1, 1] (+x right, +y up/forward)
        float stickXState = 0.0f;
        float stickYState = 0.0f;

        // Per-hand thumbsticks, in [-1, 1] (+x right, +y up/forward)
        float leftStickX = 0.0f;
        float leftStickY = 0.0f;
        float rightStickX = 0.0f;
        float rightStickY = 0.0f;

        // Edge-triggered stick "presses" (deflection beyond a threshold), either hand
        bool leftPressed = false;
        bool rightPressed = false;
        bool upPressed = false;
        bool downPressed = false;
    };
} /*namespace sibr*/
