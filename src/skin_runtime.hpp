#pragma once
namespace skins {
// MinHook is initialized by the overlay. Missing CS2 modules leave the frontend available.
void install_runtime();
// Restores tracked local items on the game callback before removing its hook.
bool stop_runtime();
}
