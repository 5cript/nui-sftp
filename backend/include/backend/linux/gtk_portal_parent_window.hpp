#pragma once

#include <string>

/**
 * @brief Parent window identifier for xdg-desktop-portal calls of a WebKitGTK window.
 * @details On Wayland the surface is exported through GDK (zxdg_exporter_v2), the export stays alive so the handle
 *          remains valid. On X11 it is the window id.
 * @param nativeWindow The GtkWidget* of the window.
 * @return "wayland:<handle>", "x11:<id>", or empty when there is none, then the portal shows its dialog without parent.
 */
std::string gtkPortalParentWindow(void* nativeWindow);
