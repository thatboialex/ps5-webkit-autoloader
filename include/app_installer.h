#pragma once

/* Launcher bitmask used by the installer. */
#define WKALI_LAUNCHER_UMTX2   (1u << 0)
#define WKALI_LAUNCHER_POOPS   (1u << 1)
#define WKALI_LAUNCHER_RELAPSE (1u << 2)

/* Installs/updates every launcher selected by launcher_mask. */
int wkali_install_apps_if_needed(unsigned int launcher_mask);
