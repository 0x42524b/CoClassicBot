#pragma once

// Initialize the ImGui D3D11 overlay (hooks IDXGISwapChain::Present + WndProc).
// Call from a background thread after the game window + device exist.
void InitOverlay();

// Shutdown overlay: unhook Present + restore WndProc.
void ShutdownOverlay();

// Returns true if overlay is currently visible.
bool IsOverlayVisible();
