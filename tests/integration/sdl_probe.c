/*
 * Checks that SDL's Valve-authored Steam Controller (Triton) driver accepts
 * the virtual controller: correct type, both touchpads, gyro, and live input.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Gamepad *gamepad = NULL;
    const Uint64 deadline = SDL_GetTicks() + 20000;
    while (!gamepad && SDL_GetTicks() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_Gamepad *candidate = SDL_OpenGamepad(event.gdevice.which);
                if (candidate && SDL_GetGamepadVendor(candidate) == 0x28DE && SDL_GetGamepadProduct(candidate) == 0x1302) {
                    gamepad = candidate;
                } else if (candidate) {
                    SDL_CloseGamepad(candidate);
                }
            }
        }
        SDL_Delay(50);
    }
    if (!gamepad) {
        printf("FAIL: SDL did not report a 28DE:1302 gamepad\n");
        return 1;
    }

    const char *name = SDL_GetGamepadName(gamepad);
    const int touchpads = SDL_GetNumGamepadTouchpads(gamepad);
    const bool gyro = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO);
    printf("name='%s' touchpads=%d gyro=%d\n", name ? name : "(null)", touchpads, gyro);
    if (gyro) {
        SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true);
    }

    Sint16 min_x = 32767, max_x = -32768;
    int a_presses = 0, touch_samples = 0;
    bool last_a = false;
    const Uint64 end = SDL_GetTicks() + 3000;
    while (SDL_GetTicks() < end) {
        SDL_UpdateGamepads();
        const Sint16 x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        min_x = x < min_x ? x : min_x;
        max_x = x > max_x ? x : max_x;
        const bool a = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
        a_presses += (a && !last_a) ? 1 : 0;
        last_a = a;
        bool down = false;
        if (touchpads > 1 && SDL_GetGamepadTouchpadFinger(gamepad, 1, 0, &down, NULL, NULL, NULL) && down) {
            ++touch_samples;
        }
        SDL_Delay(4);
    }
    printf("left x range [%d, %d], A presses %d, right-pad touch samples %d\n", min_x, max_x, a_presses, touch_samples);

    const bool ok = name && strstr(name, "Steam Controller") && touchpads == 2 && gyro &&
                    (max_x - min_x) > 20000 && a_presses >= 2 && touch_samples > 10;
    printf("%s\n", ok ? "PASS" : "FAIL");
    SDL_CloseGamepad(gamepad);
    SDL_Quit();
    return ok ? 0 : 1;
}
