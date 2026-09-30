/*
 * PS5 Homescreen App Installer for the WebKit Autoloader Installer.
 * Based on the original implementation in ftpsrv by John Törnblom
 * and Payload Manager by PLK.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_installer.h"
#include "wkali.h"
#include <ps5/kernel.h>

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                            \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"            \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const uint8_t name[];                                                 \
  extern const size_t name##_size;

INCASSET(param_json, "assets/param.json");
INCASSET(icon0_png, "assets/icon0.png");
INCASSET(param_poops_json, "assets/param-poops.json");
INCASSET(icon0_poops_png, "assets/icon0-poops.png");
INCASSET(param_relapse_json, "assets/param-relapse.json");
INCASSET(icon0_relapse_png, "assets/icon0-relapse.png");

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

typedef struct {
  const char *title_id;
  const char *display_name;
  const uint8_t *param_data;
  const size_t *param_size;
  const uint8_t *icon_data;
  const size_t *icon_size;
} LauncherDefinition;

static const LauncherDefinition launcher_umtx2 = {
    WKAL_TITLE_ID,
    "WebKit Autoloader",
    param_json,
    &param_json_size,
    icon0_png,
    &icon0_png_size,
};

static const LauncherDefinition launcher_poops = {
    WKAL_POOPS_TITLE_ID,
    "WebKit Autoloader - Poops",
    param_poops_json,
    &param_poops_json_size,
    icon0_poops_png,
    &icon0_poops_png_size,
};

static const LauncherDefinition launcher_relapse = {
    WKAL_RELAPSE_TITLE_ID,
    "WebKit Autoloader - Relapse",
    param_relapse_json,
    &param_relapse_json_size,
    icon0_relapse_png,
    &icon0_relapse_png_size,
};

/* Path buffers below are built as /user/app/<title_id>/... */
_Static_assert(sizeof(WKAL_TITLE_ID) <= 16, "WKAL_TITLE_ID too long");
_Static_assert(sizeof(WKAL_POOPS_TITLE_ID) <= 16, "WKAL_POOPS_TITLE_ID too long");
_Static_assert(sizeof(WKAL_RELAPSE_TITLE_ID) <= 16, "WKAL_RELAPSE_TITLE_ID too long");

static int mkdir_p(const char *path, mode_t mode) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (len == 0)
    return 0;
  if (tmp[len - 1] == '/')
    tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

static int install_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f;
  if (!(f = fopen(path, "wb")))
    return -1;
  if (fwrite(data, size, 1, f) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int install_app(const char *title_id, const char *dir) {
  int (*sceAppInstUtilAppInstallTitleDir)(const char *, const char *, void *) = 0;
  const char *nid = "Wudg3Xe3heE";
  uint32_t handle;

  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    sceAppInstUtilAppInstallTitleDir =
        (void *)kernel_dynlib_resolve(-1, handle, nid);
  }

  if (sceAppInstUtilAppInstallTitleDir)
    return sceAppInstUtilAppInstallTitleDir(title_id, dir, 0);

  return sceAppInstUtilAppInstallAll(0);
}

static int needs_update(const char *path, const uint8_t *expected_data,
                        size_t expected_size) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 1;
  if ((size_t)st.st_size != expected_size)
    return 1;

  FILE *f = fopen(path, "rb");
  if (!f)
    return 1;

  uint8_t *buf = malloc(expected_size);
  if (!buf) {
    fclose(f);
    return 1;
  }

  if (fread(buf, 1, expected_size, f) != expected_size) {
    free(buf);
    fclose(f);
    return 1;
  }
  fclose(f);

  int mismatch = memcmp(buf, expected_data, expected_size);
  free(buf);
  return mismatch != 0;
}

static void remove_legacy_generic_launcher_if_present(void) {
  char legacy_dir[256];
  struct stat st;
  snprintf(legacy_dir, sizeof(legacy_dir), "/user/app/%s", WKAL_TITLE_ID);
  if (stat(legacy_dir, &st) != 0)
    return;

  int err = sceAppInstUtilAppUnInstall(WKAL_TITLE_ID);
  if (err) {
    wkali_log("[WKALI] Legacy generic launcher cleanup failed: 0x%08X\n", err);
  } else {
    wkali_log("[WKALI] Removed legacy generic launcher (%s).\n", WKAL_TITLE_ID);
  }
}

static int install_launcher_if_needed(const LauncherDefinition *launcher) {
  char base_dir[256];
  char param_path[256];
  char icon_path[256];

  snprintf(base_dir, sizeof(base_dir), "/user/app/%s", launcher->title_id);
  snprintf(param_path, sizeof(param_path), "%s/sce_sys/param.json", base_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/sce_sys/icon0.png", base_dir);

  int update_needed = 0;
  struct stat st;
  if (stat(base_dir, &st) != 0) {
    update_needed = 1;
  } else {
    if (needs_update(param_path, launcher->param_data, *launcher->param_size))
      update_needed = 1;
    if (needs_update(icon_path, launcher->icon_data, *launcher->icon_size))
      update_needed = 1;
  }

  if (!update_needed) {
    wkali_log("[WKALI] %s (%s) is already up to date.\n",
              launcher->display_name, launcher->title_id);
    return 0;
  }

  if (stat(base_dir, &st) == 0) {
    wkali_log("[WKALI] Updating %s (%s)...\n",
              launcher->display_name, launcher->title_id);
  } else {
    wkali_log("[WKALI] Installing %s (%s)...\n",
              launcher->display_name, launcher->title_id);
  }
  wkali_notify("Installing %s...", launcher->display_name);

  char sce_sys_dir[256];
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", base_dir);
  if (mkdir_p(sce_sys_dir, 0755) != 0) {
    wkali_log("[WKALI] Failed to create app dir: %s (errno: %d)\n",
              sce_sys_dir, errno);
    return -1;
  }

  if (install_file(param_path, launcher->param_data, *launcher->param_size)) {
    wkali_log("[WKALI] Failed to install %s param.json\n",
              launcher->display_name);
    return -1;
  }

  if (install_file(icon_path, launcher->icon_data, *launcher->icon_size)) {
    wkali_log("[WKALI] Failed to install %s icon0.png\n",
              launcher->display_name);
    return -1;
  }

  if (install_app(launcher->title_id, "/user/app/")) {
    wkali_log("[WKALI] install_app failed for %s (%s)\n",
              launcher->display_name, launcher->title_id);
    return -1;
  }

  wkali_log("[WKALI] %s installed successfully.\n", launcher->display_name);
  return 0;
}

int wkali_install_apps_if_needed(unsigned int launcher_mask) {
  if (launcher_mask == 0) {
    wkali_log("[WKALI] No compatible launcher selected for installation.\n");
    return -1;
  }

  int err = sceAppInstUtilInitialize();
  if (err) {
    wkali_log("[WKALI] sceAppInstUtilInitialize: error 0x%08X\n", err);
    return -1;
  }

  int result = 0;
  if ((launcher_mask & WKALI_LAUNCHER_UMTX2) &&
      install_launcher_if_needed(&launcher_umtx2) != 0)
    result = -1;
  if ((launcher_mask & WKALI_LAUNCHER_POOPS) &&
      install_launcher_if_needed(&launcher_poops) != 0)
    result = -1;
  if ((launcher_mask & WKALI_LAUNCHER_RELAPSE) &&
      install_launcher_if_needed(&launcher_relapse) != 0)
    result = -1;

  sceAppInstUtilTerminate();

  if (result == 0) {
    /* Migrating a 7.xx+ install from the old single generic shortcut should
     * leave only the dedicated launcher(s). UMTX2 still owns WKAL00001. */
    if ((launcher_mask & WKALI_LAUNCHER_UMTX2) == 0)
      remove_legacy_generic_launcher_if_present();

    wkali_log("[WKALI] Requested launcher set installed successfully.\n");
    wkali_notify("WebKit Autoloader launchers ready!");
  }
  return result;
}
