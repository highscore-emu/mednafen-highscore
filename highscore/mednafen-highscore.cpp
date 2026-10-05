#include <mednafen/mednafen.h>
#include <mednafen/state-driver.h>

#include "mednafen-highscore.h"

#include <mednafen/pce_fast/pce.h>
#include <mednafen/pce_fast/vdc.h>

#define SOUND_BUFFER_SIZE 0x10000
#define SAMPLE_RATE 44100

static MednafenCore *core;

struct _MednafenCore
{
  HsCore parent_instance;

  Mednafen::MDFNGI *game;
  Mednafen::MDFN_Surface *surface;

  HsSoftwareContext *context;
  uint8_t *frame_buffer;

  uint32_t *input_buffer[13];
  int16_t *sound_buffer;

  char *rom_path;
  GFile *m3u_file;

  guint current_disc;
  guint media_cb_id;
  guint media_length;

  gboolean psx_ds_analog[4];

  int ss_reset_counter;
  HsSegaSaturnController ss_controller_type[12];

  gboolean pce_use_sgx;

  float colorburst_offset;

  int top_overscan_n;
  int bottom_overscan_n;

  int top_overscan_p;
  int bottom_overscan_p;
};

static void mednafen_atari_lynx_core_init (HsAtariLynxCoreInterface *iface);
static void mednafen_neo_geo_pocket_core_init (HsNeoGeoPocketCoreInterface *iface);
static void mednafen_neo_geo_pocket_color_core_init (HsNeoGeoPocketColorCoreInterface *iface);
static void mednafen_pc_engine_core_init (HsPcEngineCoreInterface *iface);
static void mednafen_pc_engine_cd_core_init (HsPcEngineCdCoreInterface *iface);
static void mednafen_playstation_core_init (HsPlayStationCoreInterface *iface);
static void mednafen_sega_saturn_core_init (HsSegaSaturnCoreInterface *iface);
static void mednafen_virtual_boy_core_init (HsVirtualBoyCoreInterface *iface);
static void mednafen_wonderswan_core_init (HsWonderSwanCoreInterface *iface);
static void mednafen_wonderswan_color_core_init (HsWonderSwanColorCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (MednafenCore, mednafen_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_ATARI_LYNX_CORE, mednafen_atari_lynx_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_NEO_GEO_POCKET_CORE, mednafen_neo_geo_pocket_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_NEO_GEO_POCKET_COLOR_CORE, mednafen_neo_geo_pocket_color_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_PC_ENGINE_CORE, mednafen_pc_engine_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_PC_ENGINE_CD_CORE, mednafen_pc_engine_cd_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_PLAYSTATION_CORE, mednafen_playstation_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_SEGA_SATURN_CORE, mednafen_sega_saturn_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_VIRTUAL_BOY_CORE, mednafen_virtual_boy_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_WONDERSWAN_CORE, mednafen_wonderswan_core_init)
                               G_IMPLEMENT_INTERFACE (HS_TYPE_WONDERSWAN_COLOR_CORE, mednafen_wonderswan_color_core_init))

void
Mednafen::MDFND_OutputInfo (const char *s) noexcept
{
  g_autofree char *message = g_strdup (s);

  // Trim the unwanted newline
  int len = strlen (message);
  if (message[len - 1] == '\n')
    message[len - 1] = '\0';

  hs_core_log_literal (HS_CORE (core), HS_LOG_INFO, message);
}

void
Mednafen::MDFND_OutputNotice (MDFN_NoticeType t, const char* s) noexcept
{
  HsLogLevel level;

  switch (t) {
  case MDFN_NOTICE_STATUS:
    level = HS_LOG_DEBUG;
    break;
  case MDFN_NOTICE_WARNING:
    level = HS_LOG_WARNING;
    break;
  case MDFN_NOTICE_ERROR:
    level = HS_LOG_CRITICAL;
    break;
  default:
    g_assert_not_reached ();
  }

  hs_core_log_literal (HS_CORE (core), level, s);
}

void
Mednafen::MDFND_MediaSetNotification(uint32 drive_idx, uint32 state_idx, uint32 media_idx, uint32 orientation_idx)
{
  if (state_idx == 0 || media_idx == core->current_disc)
    return;

  core->current_disc = media_idx;
  hs_core_notify_current_media (HS_CORE (core));
}

static GFile *
make_m3u (MednafenCore  *core,
          const char   **rom_paths,
          int            n_rom_paths,
          GError       **error)
{
  g_autoptr (GFileIOStream) iostream = NULL;
  GOutputStream *ostream;
  GDataOutputStream *stream;
  GFile *ret;
  int i;

  ret = g_file_new_tmp ("mednafen_highscore_XXXXXX.m3u", &iostream, error);
  if (!ret)
    return NULL;

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (iostream));
  stream = g_data_output_stream_new (ostream);

  for (i = 0; i < n_rom_paths; i++) {
    g_data_output_stream_put_string (stream, rom_paths[i], NULL, error);
    g_data_output_stream_put_byte (stream, '\n', NULL, error);
  }

  g_io_stream_close (G_IO_STREAM (iostream), NULL, NULL);

  return ret;
}

static void
setup_controllers (MednafenCore *self)
{
  HsPlatform platform = hs_core_get_platform (HS_CORE (self));
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  switch (base_platform) {
  case HS_PLATFORM_ATARI_LYNX:
    self->game->SetInput (0, "gamepad", (uint8_t *) self->input_buffer[0]);
    break;
  case HS_PLATFORM_NEO_GEO_POCKET:
    self->game->SetInput (0, "gamepad", (uint8_t *) self->input_buffer[0]);
    break;
  case HS_PLATFORM_PC_ENGINE:
    self->game->SetInput (0, "gamepad", (uint8_t *) self->input_buffer[0]);
    self->game->SetInput (1, "gamepad", (uint8_t *) self->input_buffer[1]);
    self->game->SetInput (2, "gamepad", (uint8_t *) self->input_buffer[2]);
    self->game->SetInput (3, "gamepad", (uint8_t *) self->input_buffer[3]);
    self->game->SetInput (4, "gamepad", (uint8_t *) self->input_buffer[4]);
    break;
  case HS_PLATFORM_PLAYSTATION:
    for (int i = 0; i < 4; i++) {
      self->game->SetInput (i, "dualshock", (uint8_t *) self->input_buffer[i]);
    }
    break;
  case HS_PLATFORM_SEGA_SATURN:
    for (int i = 0; i < 12; i++) {
      self->ss_controller_type[i] = HS_SEGA_SATURN_CONTROL_PAD;
      self->game->SetInput (i, "gamepad", (uint8_t *) self->input_buffer[i]);
    }
    self->game->SetInput (12, "builtin", (uint8_t *) self->input_buffer[12]); // reset button status
    break;
  case HS_PLATFORM_VIRTUAL_BOY:
    self->game->SetInput (0, "gamepad", (uint8_t *) self->input_buffer[0]);
    self->game->SetInput (1, "misc", (uint8_t *) self->input_buffer[1]); // TODO use this
    break;
  case HS_PLATFORM_WONDERSWAN:
    self->game->SetInput (0, "gamepad", (uint8_t *) self->input_buffer[0]);
    break;
  default:
    g_assert_not_reached ();
  }
}

static gboolean
reload_game (MednafenCore *self, GError **error)
{
  HsPlatform platform = hs_core_get_platform (HS_CORE (self));
  g_autofree char *system_name = g_strdup (self->game->shortname);
  Mednafen::MDFNI_CloseGame ();
  self->game = Mednafen::MDFNI_LoadGame (system_name, &::Mednafen::NVFS, self->rom_path);
  if (!self->game) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load game");
    return FALSE;
  }

  setup_controllers (self);

  if (platform == HS_PLATFORM_PC_ENGINE_CD ||
      platform == HS_PLATFORM_PLAYSTATION ||
      platform == HS_PLATFORM_SEGA_SATURN) {
    Mednafen::MDFNI_SetMedia (0, 2, self->current_disc, 0);
  }

  return TRUE;
}

static gboolean
try_migrate_libretro_save (MednafenCore  *self,
                           const char    *save_path,
                           GError       **error)
{
  HsPlatform platform = hs_core_get_platform (HS_CORE (self));
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  if (base_platform == HS_PLATFORM_PLAYSTATION) {
    g_autoptr (GFile) save_file = g_file_new_for_path (save_path);

    if (!g_file_query_exists (save_file, NULL))
      return TRUE;

    if (g_file_query_file_type (save_file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY)
      return TRUE;

    // Make a temporary file
    g_autofree char *cache_path = hs_core_get_cache_path (HS_CORE (self));
    g_autoptr (GFile) cache_dir = g_file_new_for_path (cache_path);
    if (!g_file_query_exists (cache_dir, NULL) &&
        !g_file_make_directory_with_parents (cache_dir, NULL, error)) {
      return FALSE;
    }

    g_autofree char *tmp_path = g_build_filename (cache_path, "mednafen-save-XXXXXX", NULL);
    tmp_path = g_mkdtemp (tmp_path);
    g_autoptr (GFile) tmp_file = g_file_new_for_path (tmp_path);

    // Move the old save, replace it with a directory
    g_autoptr (GFile) tmp_save_file = g_file_get_child (tmp_file, "save");
    if (!g_file_move (save_file, tmp_save_file, G_FILE_COPY_BACKUP, NULL, NULL, NULL, error))
      return FALSE;

    if (!g_file_make_directory_with_parents (save_file, NULL, error))
      return FALSE;

    g_autoptr (GFile) dest_file = g_file_get_child (save_file, "save.0.mcr");
    if (!g_file_move (tmp_save_file, dest_file, G_FILE_COPY_BACKUP, NULL, NULL, NULL, error))
      return FALSE;

    if (!g_file_delete (tmp_file, NULL, error))
      return FALSE;

    hs_core_log (HS_CORE (self), HS_LOG_MESSAGE, "Libretro save files migrated successfully");

    return TRUE;
  }

  if (base_platform == HS_PLATFORM_SEGA_SATURN) {
    g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);
    g_autoptr (GFileEnumerator) enumerator = NULL;
    g_autoptr (GFile) bkr_file = NULL;
    g_autoptr (GFile) bkr_dest = NULL;
    g_autoptr (GFile) bcr_file = NULL;
    g_autoptr (GFile) bcr_dest = NULL;
    g_autoptr (GFile) arp_file = NULL;
    g_autoptr (GFile) arp_dest = NULL;
    g_autoptr (GFile) seep_file = NULL;
    g_autoptr (GFile) seep_dest = NULL;
    g_autoptr (GFile) smpc_file = NULL;
    g_autoptr (GFile) smpc_dest = NULL;
    GFileInfo *info;

    if (!g_file_query_exists (save_dir, NULL))
      return TRUE;

    enumerator =
      g_file_enumerate_children (save_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                 G_FILE_QUERY_INFO_NONE, NULL, error);
    if (!enumerator)
      return FALSE;

    while ((info = g_file_enumerator_next_file (enumerator, NULL, error))) {
      const char *filename = g_file_info_get_name (info);

      if (g_str_has_suffix (filename, ".bkr") && g_strcmp0 (filename, "save.bkr") && !bkr_file) {
        bkr_file = g_file_get_child (save_dir, filename);
        bkr_dest = g_file_get_child (save_dir, "save.bkr");
      } else if (g_str_has_suffix (filename, ".bcr") && g_strcmp0 (filename, "save.bcr") && !bcr_file) {
        bcr_file = g_file_get_child (save_dir, filename);
        bcr_dest = g_file_get_child (save_dir, "save.bcr");
      } else if (g_str_has_suffix (filename, ".arp") && g_strcmp0 (filename, "save.arp") && !arp_file) {
        arp_file = g_file_get_child (save_dir, filename);
        arp_dest = g_file_get_child (save_dir, "save.arp");
      } else if (g_str_has_suffix (filename, ".seep") && g_strcmp0 (filename, "save.seep") && !seep_file) {
        seep_file = g_file_get_child (save_dir, filename);
        seep_dest = g_file_get_child (save_dir, "save.seep");
      } else if (g_str_has_suffix (filename, ".smpc") && g_strcmp0 (filename, "save.smpc") && !smpc_file) {
        smpc_file = g_file_get_child (save_dir, filename);
        smpc_dest = g_file_get_child (save_dir, "save.smpc");
      }

      g_object_unref (info);
    }

    if (bkr_file && !g_file_move (bkr_file, bkr_dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
      return FALSE;
    if (bcr_file && !g_file_move (bcr_file, bcr_dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
      return FALSE;
    if (arp_file && !g_file_move (arp_file, arp_dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
      return FALSE;
    if (seep_file && !g_file_move (seep_file, seep_dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
      return FALSE;
    if (smpc_file && !g_file_move (smpc_file, smpc_dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
      return FALSE;

    if (bkr_file || bcr_file || arp_file || seep_file || smpc_file)
      hs_core_log (HS_CORE (self), HS_LOG_MESSAGE, "Libretro save files migrated successfully");

    return TRUE;
  }

  return TRUE;
}

static gboolean
set_save_path (MednafenCore  *self,
               const char    *save_path,
               GError       **error)
{
  HsPlatform platform = hs_core_get_platform (HS_CORE (self));
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  if (base_platform == HS_PLATFORM_PLAYSTATION ||
      base_platform == HS_PLATFORM_SEGA_SATURN) {
    g_autofree char *path_with_ext = g_build_filename (save_path, "save.%x", NULL);
    g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);

    if (!g_file_query_exists (save_dir, NULL) &&
        !g_file_make_directory_with_parents (save_dir, NULL, error)) {
      return FALSE;
    }

    Mednafen::MDFNI_SetSetting ("filesys.fname_sav", path_with_ext);
    return TRUE;
  }

  Mednafen::MDFNI_SetSetting ("filesys.fname_sav", save_path);
  return TRUE;
}

static gboolean
mednafen_core_load_rom (HsCore      *core,
                        const char **rom_paths,
                        int          n_rom_paths,
                        const char  *save_path,
                        GError     **error)
{
  MednafenCore *self = MEDNAFEN_CORE (core);
  HsPlatform platform = hs_core_get_platform (core);
  HsPlatform base_platform = hs_platform_get_base_platform (platform);
  const char *rom_path;

  if (!try_migrate_libretro_save (self, save_path, error))
    return FALSE;

  if (!Mednafen::MDFNI_Init ()) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to initialize Mednafen");
    return FALSE;
  }

  g_autofree char *cache_dir = hs_core_get_cache_path (core);
  if (!Mednafen::MDFNI_InitFinalize (cache_dir)) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to finish initializing Mednafen");
    return FALSE;
  }

  Mednafen::MDFNI_SetSetting ("filesys.path_sav", "");
  Mednafen::MDFNI_SetSetting ("video.deinterlacer", "bob");

  if (!set_save_path (self, save_path, error))
    return FALSE;

  if (platform == HS_PLATFORM_ATARI_LYNX) {
    const char *bios_path = hs_core_query_firmware_path (core, HS_ATARI_LYNX_FIRMWARE_BOOT_ROM);

    if (!bios_path) {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_MISSING_FIRMWARE, "Missing Lynx boot ROM");
      return FALSE;
    }

    Mednafen::MDFNI_SetSetting ("lynx.bios", bios_path);
    Mednafen::MDFNI_SetSetting ("lynx.rotateinput", "0");
  }

  if (base_platform == HS_PLATFORM_PC_ENGINE) {
    Mednafen::MDFNI_SetSetting ("pce_fast.slstart", "0");
    Mednafen::MDFNI_SetSetting ("pce_fast.slend", "239");
    Mednafen::MDFNI_SetSetting ("pce_fast.forcesgx", self->pce_use_sgx ? "1" : "0");

    self->top_overscan_n = 4;
    self->bottom_overscan_n = 4;

    self->top_overscan_p = 4;
    self->bottom_overscan_p = 4;
  }

  if (platform == HS_PLATFORM_PC_ENGINE_CD) {
    const char *bios_path = hs_core_query_firmware_path (core, HS_PC_ENGINE_CD_FIRMWARE_SYSTEM_CARD_3_0);

    if (!bios_path) {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_MISSING_FIRMWARE, "Missing System Card 3.0 BIOS");
      return FALSE;
    }

    Mednafen::MDFNI_SetSetting ("pce_fast.cdbios", bios_path);
  }

  if (platform == HS_PLATFORM_PLAYSTATION) {
    const char *jp_path = hs_core_query_firmware_path (core, HS_PLAYSTATION_FIRMWARE_JAPAN);
    const char *na_path = hs_core_query_firmware_path (core, HS_PLAYSTATION_FIRMWARE_NORTH_AMERICA);
    const char *eu_path = hs_core_query_firmware_path (core, HS_PLAYSTATION_FIRMWARE_EUROPE);

    // We don't want to count all 3 as used
    hs_core_reset_used_firmware (core);

    if (jp_path)
      Mednafen::MDFNI_SetSetting ("psx.bios_jp", jp_path);
    if (na_path)
      Mednafen::MDFNI_SetSetting ("psx.bios_na", na_path);
    if (eu_path)
      Mednafen::MDFNI_SetSetting ("psx.bios_eu", eu_path);

    Mednafen::MDFNI_SetSetting ("psx.h_overscan", "0");

    self->top_overscan_n = 8;
    self->bottom_overscan_n = 8;

    self->top_overscan_p = 24;
    self->bottom_overscan_p = 24;
  }

  if (platform == HS_PLATFORM_SEGA_SATURN) {
    const char *jp_path = hs_core_query_firmware_path (core, HS_SEGA_SATURN_FIRMWARE_JAPAN);
    const char *na_eu_path = hs_core_query_firmware_path (core, HS_SEGA_SATURN_FIRMWARE_OVERSEAS);

    // We don't want to count both as used
    hs_core_reset_used_firmware (core);

    if (jp_path)
      Mednafen::MDFNI_SetSetting ("ss.bios_jp", jp_path);
    if (na_eu_path)
      Mednafen::MDFNI_SetSetting ("ss.bios_na_eu", na_eu_path);

    Mednafen::MDFNI_SetSetting ("ss.h_overscan", "0");

    Mednafen::MDFNI_SetSetting ("ss.slstartp", "-16");
    Mednafen::MDFNI_SetSetting ("ss.slendp", "271");

    self->top_overscan_n = 8;
    self->bottom_overscan_n = 8;

    self->top_overscan_p = 24;
    self->bottom_overscan_p = 24;
  }

  if (platform == HS_PLATFORM_PC_ENGINE_CD ||
      platform == HS_PLATFORM_PLAYSTATION ||
      platform == HS_PLATFORM_SEGA_SATURN) {
    if (n_rom_paths > 1) {
      // Make m3u work
      Mednafen::MDFNI_SetSetting ("filesys.untrusted_fip_check", "0");

      self->m3u_file = make_m3u (self, rom_paths, n_rom_paths, error);
      if (!self->m3u_file)
        return FALSE;

      rom_path = g_file_peek_path (self->m3u_file);
    } else {
      rom_path = rom_paths[0];
    }
  } else {
    g_assert (n_rom_paths == 1);
    rom_path = rom_paths[0];
  }

  const char *platform_name;

  switch (base_platform) {
  case HS_PLATFORM_ATARI_LYNX:
    platform_name = "lynx";
    break;
  case HS_PLATFORM_NEO_GEO_POCKET:
    platform_name = "ngp";
    break;
  case HS_PLATFORM_PC_ENGINE:
    platform_name = "pce_fast";
    break;
  case HS_PLATFORM_PLAYSTATION:
    platform_name = "psx";
    break;
  case HS_PLATFORM_SEGA_SATURN:
    platform_name = "ss";
    break;
  case HS_PLATFORM_VIRTUAL_BOY:
    platform_name = "vb";
    break;
  case HS_PLATFORM_WONDERSWAN:
    platform_name = "wswan";
    break;
  default:
    g_assert_not_reached ();
  }

  self->game = Mednafen::MDFNI_LoadGame (platform_name, &::Mednafen::NVFS, rom_path);

  if (base_platform == HS_PLATFORM_PLAYSTATION) {
    std::string bios = Mednafen::MDFN_GetSettingS ("psx.used_bios");
    HsPlayStationFirmware id;
    const char *region;

    if (bios == "psx.bios_jp") {
      id = HS_PLAYSTATION_FIRMWARE_JAPAN;
      region = "JP";
    } else if (bios == "psx.bios_na") {
      id = HS_PLAYSTATION_FIRMWARE_NORTH_AMERICA;
      region = "US";
    } else if (bios == "psx.bios_eu") {
      id = HS_PLAYSTATION_FIRMWARE_EUROPE;
      region = "EU";
    } else {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load game");
      return FALSE;
    }

    if (!hs_core_query_firmware_path (core, id)) {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_MISSING_FIRMWARE, "Missing Playstation %s BIOS", region);
      return FALSE;
    }
  }

  if (base_platform == HS_PLATFORM_SEGA_SATURN) {
    std::string bios = Mednafen::MDFN_GetSettingS ("ss.used_bios");
    HsSegaSaturnFirmware id;
    const char *region;

    if (bios == "ss.bios_jp") {
      id = HS_SEGA_SATURN_FIRMWARE_JAPAN;
      region = "JP";
    } else if (bios == "ss.bios_na_eu") {
      id = HS_SEGA_SATURN_FIRMWARE_OVERSEAS;
      region = "US / EU";
    } else {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load game");
      return FALSE;
    }

    if (!hs_core_query_firmware_path (core, id)) {
      g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_MISSING_FIRMWARE, "Missing Sega Saturn %s BIOS", region);
      return FALSE;
    }
  }

  if (!self->game) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load game");
    return FALSE;
  }

  int w = self->game->multires ? self->game->lcm_width : self->game->fb_width;
  int h = self->game->multires ? self->game->lcm_height : self->game->fb_height;

  self->context = hs_core_create_software_context (core, w, h, HS_PIXEL_FORMAT_B8G8R8X8);
  self->frame_buffer = g_new0 (guint8, self->game->fb_width * self->game->fb_height * 4);

  self->surface = new Mednafen::MDFN_Surface (self->frame_buffer,
                                              self->game->fb_width, self->game->fb_height, self->game->fb_width,
                                              Mednafen::MDFN_PixelFormat::ARGB32_8888);

  setup_controllers (self);

  self->rom_path = g_strdup (rom_path);
  self->media_length = n_rom_paths;

  if (platform == HS_PLATFORM_PC_ENGINE_CD ||
      platform == HS_PLATFORM_PLAYSTATION ||
      platform == HS_PLATFORM_SEGA_SATURN) {
    Mednafen::MDFNI_SetMedia (0, 2, 0, 0);
  }

  return TRUE;
}

const int LYNX_BUTTON_MAPPING[] = {
  6, 7, 4, 5, // UP, DOWN, LEFT, RIGHT
  0, 1, 3, 2, // A, B, OPTION1, OPTION2
  8,          // PAUSE
};

const int NGP_BUTTON_MAPPING[] = {
  0, 1, 2, 3,  // UP, DOWN, LEFT, RIGHT
  4, 5, 6,     // A, B, OPTION
};

const int PCE_BUTTON_MAPPING[] = {
  4, 6, 7,  5,  // UP, DOWN, LEFT, RIGHT
  0, 1,         // I, II
  8, 9, 10, 11, // III, IV, V, VI
  2, 3          // SELECT, RUN
};

#define PCE_MODE_SWITCH_MASK (1 << 12)

const int PSX_BUTTON_MAPPING[] = {
  4,  6,  7,  5,  // UP, DOWN, LEFT, RIGHT
  12, 15, 13, 14, // TRIANGLE, SQUARE, CIRCLE, CROSS
  10, 8,  1,      // L1, L2, L3
  11, 9,  2,      // R1, R2, R3
  0,  3,          // SELECT, START
};

#define PSX_MODE_SWITCH_MASK (1 << 16)

#define PSX_LOCKED_STATUS_MASK (1 << 18)
#define PSX_MODE_STATUS_MASK (1 << 17)

const int PSX_STICK_MAPPING[] = {
  7, 9, // L(x, y)
  3, 5, // R(x, y)
};

const int SS_BUTTON_MAPPING[] = {
  4,  5, 6, 7, // UP, DOWN, LEFT, RIGHT
  10, 8, 9,    // A, B, C
  2,  1, 0,    // X, Y, Z
  15, 3, 11,   // L, R, START
};

const int SS_3D_BUTTON_MAPPING[] = {
  0, 1, 2, 3,  // UP, DOWN, LEFT, RIGHT
  6, 4, 5,     // A, B, C
  10, 9, 8,    // X, Y, Z
  -1, -1, 7    // L, R, START
};

#define SS_3D_MODE_SWITCH_MASK (1 << 12)
#define SS_3D_STICK_X 2
#define SS_3D_STICK_Y 4
#define SS_3D_STICK_DEADZONE 0.05
#define SS_3D_TRIGGER_L 8
#define SS_3D_TRIGGER_R 6

const int VB_BUTTON_MAPPING[] = {
  9,  8,  7,  6,  // L_UP, L_DOWN, L_LEFT, L_RIGHT
  4,  13, 12, 5,  // R_UP, R_DOWN, R_LEFT, R_RIGHT
  0,  1,  11, 10, // A,    B,      SELECT, START
  2,  3,          // L,    R
};

const int WS_BUTTON_MAPPING[] = {
  0, 1,  2, 3, // X1, X2, X3, X4
  4, 5,  6, 7, // Y1, Y2, Y3, Y4
  9, 10, 8,    // A, B, START
};

static void
mednafen_core_poll_input (HsCore *core, HsInputState *input_state)
{
  MednafenCore *self = MEDNAFEN_CORE (core);
  HsPlatform platform = hs_core_get_platform (core);
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  if (base_platform == HS_PLATFORM_ATARI_LYNX) {
    uint32 buttons = input_state->atari_lynx.buttons;

    for (int btn = 0; btn < HS_ATARI_LYNX_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        *self->input_buffer[0] |= 1 << LYNX_BUTTON_MAPPING[btn];
      else
        *self->input_buffer[0] &= ~(1 << LYNX_BUTTON_MAPPING[btn]);
    }

    return;
  }

  if (base_platform == HS_PLATFORM_NEO_GEO_POCKET) {
    uint32 buttons = input_state->neo_geo_pocket.buttons;

    for (int btn = 0; btn < HS_NEO_GEO_POCKET_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        *self->input_buffer[0] |= 1 << NGP_BUTTON_MAPPING[btn];
      else
        *self->input_buffer[0] &= ~(1 << NGP_BUTTON_MAPPING[btn]);
    }

    return;
  }

  if (base_platform == HS_PLATFORM_PC_ENGINE) {
    for (int player = 0; player < HS_PC_ENGINE_MAX_PLAYERS; player++) {
      uint32 buttons = input_state->pc_engine.pad_buttons[player];

      for (int btn = 0; btn < HS_PC_ENGINE_N_BUTTONS; btn++) {
        if (buttons & 1 << btn)
          *self->input_buffer[player] |= 1 << PCE_BUTTON_MAPPING[btn];
        else
          *self->input_buffer[player] &= ~(1 << PCE_BUTTON_MAPPING[btn]);
      }

      if (input_state->pc_engine.pad_mode[player] == HS_PC_ENGINE_SIX_BUTTONS)
        *self->input_buffer[player] |= PCE_MODE_SWITCH_MASK;
      else
        *self->input_buffer[player] &= ~PCE_MODE_SWITCH_MASK;
    }

    return;
  }

  if (base_platform == HS_PLATFORM_PLAYSTATION) {
    for (int player = 0; player < HS_PLAYSTATION_MAX_PLAYERS; player++) {
      uint32 buttons = input_state->psx.pad_buttons[player];
      uint8_t *buf = (uint8_t *) self->input_buffer[player];

      const int *button_mapping, *stick_mapping;

      button_mapping = PSX_BUTTON_MAPPING;
      stick_mapping = PSX_STICK_MAPPING;

      for (int btn = 0; btn < HS_PLAYSTATION_N_BUTTONS; btn++) {
        if (button_mapping[btn] < 0)
          continue;

        if (buttons & 1 << btn)
          *self->input_buffer[player] |= 1 << button_mapping[btn];
        else
          *self->input_buffer[player] &= ~(1 << button_mapping[btn]);
      }

      gboolean is_analog = (*self->input_buffer[player] & PSX_MODE_STATUS_MASK) > 0;

      if (is_analog != self->psx_ds_analog[player])
        *self->input_buffer[player] |= PSX_MODE_SWITCH_MASK;

      for (int stick = 0; stick < HS_PLAYSTATION_N_STICKS; stick++) {
        double x = input_state->psx.pad_sticks_x[HS_PLAYSTATION_N_STICKS * player + stick];
        double y = input_state->psx.pad_sticks_y[HS_PLAYSTATION_N_STICKS * player + stick];

        double multiplier = 1.33;
        // 30712 / cos(2*pi/8) / 32767 = 1.33
        if (x < 0)
          x = -MIN (floor (0.5 + ABS (x) * 32767 * multiplier), 32767);
        else
          x = MIN (floor (0.5 + ABS (x) * 32767 * multiplier), 32767);

        if (y < 0)
          y = -MIN (floor (0.5 + ABS (y) * 32767 * multiplier), 32767);
        else
          y = MIN (floor (0.5 + ABS (y) * 32767 * multiplier), 32767);

        Mednafen::MDFN_en16lsb (&buf[stick_mapping[stick * 2]],     x + 32767);
        Mednafen::MDFN_en16lsb (&buf[stick_mapping[stick * 2 + 1]], y + 32767);
      }
    }

    return;
  }

  if (base_platform == HS_PLATFORM_SEGA_SATURN) {
    for (int player = 0; player < HS_SEGA_SATURN_MAX_PLAYERS; player++) {
      uint32 buttons = input_state->saturn.pad_buttons[player];
      uint8_t *buf = (uint8_t *) self->input_buffer[player];

      if (self->ss_controller_type[player] == HS_SEGA_SATURN_CONTROL_PAD) {
        for (int btn = 0; btn < HS_SEGA_SATURN_N_BUTTONS; btn++) {
          if (buttons & 1 << btn)
            *self->input_buffer[player] |= 1 << SS_BUTTON_MAPPING[btn];
          else
            *self->input_buffer[player] &= ~(1 << SS_BUTTON_MAPPING[btn]);
        }
      }

      if (self->ss_controller_type[player] == HS_SEGA_SATURN_3D_CONTROL_PAD) {
        if (input_state->saturn.pad_mode[player] == HS_SEGA_SATURN_3D_PAD_ANALOG)
          *self->input_buffer[player] |= SS_3D_MODE_SWITCH_MASK;
        else
          *self->input_buffer[player] &= ~SS_3D_MODE_SWITCH_MASK;

        for (int btn = 0; btn < HS_SEGA_SATURN_N_BUTTONS; btn++) {
          if (SS_3D_BUTTON_MAPPING[btn] < 0)
            continue;

          if (buttons & 1 << btn)
            *self->input_buffer[player] |= 1 << SS_3D_BUTTON_MAPPING[btn];
          else
            *self->input_buffer[player] &= ~(1 << SS_3D_BUTTON_MAPPING[btn]);
        }

        double x = input_state->saturn.pad_stick_x[player];
        double y = input_state->saturn.pad_stick_y[player];

        double distance = sqrt (x * x + y * y);
        double angle = atan2 (y, x);

        if (distance > SS_3D_STICK_DEADZONE) {
          distance = (distance - SS_3D_STICK_DEADZONE) / (1.0 - SS_3D_STICK_DEADZONE);

          x = distance * cos (angle);
          y = distance * sin (angle);
        } else {
          x = y = 0;
        }

        Mednafen::MDFN_en16lsb (&buf[SS_3D_STICK_X], floor ((1 + x) * 32767 + 0.5));
        Mednafen::MDFN_en16lsb (&buf[SS_3D_STICK_Y], floor ((1 + y) * 32767 + 0.5));

        double l = input_state->saturn.pad_left_trigger[player];
        double r = input_state->saturn.pad_right_trigger[player];

        Mednafen::MDFN_en16lsb (&buf[SS_3D_TRIGGER_L], CLAMP(l * 65535, 0, 65535));
        Mednafen::MDFN_en16lsb (&buf[SS_3D_TRIGGER_R], CLAMP(r * 65535, 0, 65535));
      }
    }

    return;
  }

  if (base_platform == HS_PLATFORM_VIRTUAL_BOY) {
    uint32 buttons = input_state->virtual_boy.buttons;

    for (int btn = 0; btn < HS_VIRTUAL_BOY_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        *self->input_buffer[0] |= 1 << VB_BUTTON_MAPPING[btn];
      else
        *self->input_buffer[0] &= ~(1 << VB_BUTTON_MAPPING[btn]);
    }

    return;
  }

  if (base_platform == HS_PLATFORM_WONDERSWAN) {
    uint32 buttons = input_state->wonderswan.buttons;

    for (int btn = 0; btn < HS_WONDERSWAN_N_BUTTONS; btn++) {
      if (buttons & 1 << btn)
        *self->input_buffer[0] |= 1 << WS_BUTTON_MAPPING[btn];
      else
        *self->input_buffer[0] &= ~(1 << WS_BUTTON_MAPPING[btn]);
    }

    return;
  }

  g_assert_not_reached ();
}

static void
mednafen_core_run_frame (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);
  HsPlatform platform = hs_core_get_platform (core);
  HsPlatform base_platform = hs_platform_get_base_platform (platform);
  int32 rects[self->game->fb_height];
  void *fb;

  memset (rects, 0, self->game->fb_height * sizeof (int32_t));
  rects[0] = ~0;

  Mednafen::EmulateSpecStruct spec;
  spec.surface = self->surface;
  spec.SoundRate = SAMPLE_RATE;
  spec.SoundBuf = self->sound_buffer;
  spec.LineWidths = rects;
  spec.SoundBufMaxSize = SOUND_BUFFER_SIZE;
  spec.SoundVolume = 1.0;
  spec.soundmultiplier = 1.0;

  Mednafen::MDFNI_Emulate (&spec);

  fb = hs_software_context_acquire_framebuffer (self->context);

  int width = 0, stride = 0;

  // Just in case let's restrict this to PCE - Saturn and PSX are a bit too performance-sensitive to risk this
  if (self->game->multires && base_platform == HS_PLATFORM_PC_ENGINE) {
    gboolean has_multiple_widths = FALSE;

    // Check if we even have multiple widths and find the largest one, we'll align everything else to it
    for (int line = spec.DisplayRect.y + 1; line < spec.DisplayRect.y + spec.DisplayRect.h; line++) {
      if (rects[line] != rects[spec.DisplayRect.y]) {
        has_multiple_widths = TRUE;
        break;
      }
    }

    if (has_multiple_widths) {
      width = self->game->lcm_width;

      int src_stride = self->game->fb_width * 4;
      int dst_stride = self->game->lcm_width * 4;

      stride = dst_stride;

      for (int line = spec.DisplayRect.y; line < spec.DisplayRect.y + spec.DisplayRect.h; line++) {
        if (rects[line] == width) {
          // Lines that fill everything we can copy as is
          memcpy (&((uint8 *) fb)[dst_stride * line], &self->frame_buffer[src_stride * line], src_stride);
        } else {
          // Otherwise we're rescaling them to the largest possible integer scale. If we leave a pixel of padding, too bad
          int scale = (int) floorf (width / (float) rects[line]);
          int src_offset = src_stride * line + spec.DisplayRect.x * 4;
          int dst_offset = dst_stride * line + spec.DisplayRect.x * 4;
          uint8 *dest = (uint8 *) fb;

          for (int pixel = 0; pixel < rects[line]; pixel++) {
            for (int i = 0; i < scale; i++) {
              dest[dst_offset]     = self->frame_buffer[src_offset];
              dest[dst_offset + 1] = self->frame_buffer[src_offset + 1];
              dest[dst_offset + 2] = self->frame_buffer[src_offset + 2];
              dest[dst_offset + 3] = self->frame_buffer[src_offset + 3];

              dst_offset += 4;
            }

            src_offset += 4;
          }
        }
      }
    } else {
      width = rects[spec.DisplayRect.y];
      stride = self->game->fb_width * 4;

      memcpy (fb, self->frame_buffer, self->game->fb_width * self->game->fb_height * 4);
    }
  } else {
    width = spec.DisplayRect.w ?: rects[spec.DisplayRect.y];
    stride = self->game->fb_width * 4;

    memcpy (fb, self->frame_buffer, self->game->fb_width * self->game->fb_height * 4);
  }

  hs_software_context_release_framebuffer (self->context);

  int stride_multiplier = spec.InterlaceOn ? 2 : 1;

  HsRectangle rect = { spec.DisplayRect.x, spec.DisplayRect.y, width, spec.DisplayRect.h / stride_multiplier };
  hs_software_context_set_area (self->context, &rect);

  hs_software_context_set_row_stride (self->context, stride * stride_multiplier);

  if (base_platform == HS_PLATFORM_PLAYSTATION ||
      base_platform == HS_PLATFORM_SEGA_SATURN ||
      base_platform == HS_PLATFORM_PC_ENGINE) {
    HsBorder overscan;
    if (hs_core_get_region (core) > HS_REGION_PAL)
      hs_border_init_full (&overscan, self->top_overscan_p, self->bottom_overscan_p, 0, 0);
    else
      hs_border_init_full (&overscan, self->top_overscan_n, self->bottom_overscan_n, 0, 0);
    hs_software_context_set_overscan (self->context, &overscan);

    HsInterlacingMode mode;

    if (spec.InterlaceOn)
      mode = spec.InterlaceField ? HS_INTERLACING_EVEN_FIELD : HS_INTERLACING_ODD_FIELD;
    else
      mode = HS_INTERLACING_NONE;

    hs_software_context_set_interlacing (self->context, mode);

    if (base_platform == HS_PLATFORM_PC_ENGINE) {
      // https://datacrystal.tcrf.net/wiki/VDC_Programmers_Reference_(Turbo-Grafx_16)#$0400_-_CR_-_Control_Register
      uint32 cr = vce.CR;

      gboolean strip_colorburst = (cr & (1 << 7)) > 0;
      gboolean blur = (cr & (1 << 2)) > 0;

      float w = width;

      // 341 is really 341⅓
      if (width == 341)
        w = 341 + 1.0 / 3.0;

      float cycle_length = w * 3.0 / 512.0;

      if (strip_colorburst)
        cycle_length = -1;

      hs_software_context_set_colorburst (self->context, cycle_length, 0.5, 0.25 + self->colorburst_offset);

      self->colorburst_offset += (mode == HS_INTERLACING_NONE) ? 0.5 : 0.25;

      if (self->colorburst_offset > 0.9)
        self->colorburst_offset = 0;
    } else {
      if (hs_core_get_region (core) == HS_REGION_PAL) {
        hs_software_context_set_colorburst (self->context, width * 3.0 / 640.0, 0.25, self->colorburst_offset);

        self->colorburst_offset += (mode == HS_INTERLACING_NONE) ? 0.25 : 0.125;

        if (self->colorburst_offset > 0.9)
          self->colorburst_offset = 0.0;
      } else {
        hs_software_context_set_colorburst (self->context, width * 3.0 / 512.0, 0.5, self->colorburst_offset);

        self->colorburst_offset += (mode == HS_INTERLACING_NONE) ? 0.5 : 0.25;

        if (self->colorburst_offset > 0.9)
          self->colorburst_offset = 0.0;
      }
    }
  }

  hs_core_play_samples (core, self->sound_buffer, spec.SoundBufSize * self->game->soundchan);

  if (base_platform == HS_PLATFORM_SEGA_SATURN && self->ss_reset_counter > 0) {
    self->ss_reset_counter--;

    if (self->ss_reset_counter == 0)
      *self->input_buffer[12] = 0;
  }

  if (base_platform == HS_PLATFORM_PLAYSTATION) {
    for (int player = 0; player < HS_PLAYSTATION_MAX_PLAYERS; player++) {
      uint8_t *buf = (uint8_t *) self->input_buffer[player];
      gboolean is_analog = (*self->input_buffer[player] & PSX_MODE_STATUS_MASK) > 0;

      if ((*self->input_buffer[player] & PSX_MODE_SWITCH_MASK) > 0)
        *self->input_buffer[player] &= ~PSX_MODE_SWITCH_MASK;

      if (self->psx_ds_analog[player] != is_analog) {
        self->psx_ds_analog[player] = is_analog;

        hs_playstation_core_emit_dualshock_mode_changed (HS_PLAYSTATION_CORE (self), player);
      }

      double weak_rumble = (double) buf[11] / 255.0;
      double strong_rumble = (double) buf[12] / 255.0;

      hs_core_rumble (core, player, strong_rumble, weak_rumble, HS_MAX_RUMBLE_DURATION);
    }
  }
}

static gboolean
mednafen_core_reset (HsCore *core, gboolean hard, GError **error)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  if (hard) {
    HsPlatform platform = hs_core_get_platform (core);
    HsPlatform base_platform = hs_platform_get_base_platform (platform);

    if (base_platform == HS_PLATFORM_PC_ENGINE) {
      gboolean was_sgx = Mednafen::MDFN_GetSettingB ("pce_fast.forcesgx");

      if (self->pce_use_sgx != was_sgx) {
        Mednafen::MDFNI_SetSetting ("pce_fast.forcesgx", self->pce_use_sgx ? "1" : "0");

        if (!reload_game (self, error))
          return FALSE;

        self->colorburst_offset = 0;
        return TRUE;
      }
    }

    Mednafen::MDFNI_Power ();
    self->colorburst_offset = 0;
    return TRUE;
  }

  // Saturn has a reset button instead of implementing the usual reset function
  // This button has to be held for a few frames before releasing
  if (hs_core_get_platform (core) == HS_PLATFORM_SEGA_SATURN) {
    *self->input_buffer[12] = 1;
    self->ss_reset_counter = 3;
    return TRUE;
  }

  Mednafen::MDFNI_Reset ();
  return TRUE;
}

static void
mednafen_core_stop (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  Mednafen::MDFNI_CloseGame ();
  Mednafen::MDFNI_Kill ();

  g_clear_pointer (&self->rom_path, g_free);

  if (self->m3u_file) {
    g_autoptr (GError) error = NULL;

    if (!g_file_delete (self->m3u_file, NULL, &error))
      hs_core_log (HS_CORE (core), HS_LOG_WARNING, "Failed to delete the m3u file: %s", error->message);

    g_clear_object (&self->m3u_file);
  }

  g_clear_pointer (&self->frame_buffer, g_free);
}

static gboolean
mednafen_core_reload_save (HsCore      *core,
                           const char  *save_path,
                           GError    **error)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  if (!set_save_path (self, save_path, error))
    return FALSE;

  return reload_game (self, error);
}

static gboolean
mednafen_core_sync_save (HsCore  *core,
                         GError **error)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  if (self->game->SyncSave)
    self->game->SyncSave();

  return TRUE;
}

static void
mednafen_core_load_state (HsCore          *core,
                          const char      *path,
                          HsStateCallback  callback)
{
  MednafenCore *self = MEDNAFEN_CORE (core);
  HsPlatform platform = hs_core_get_platform (core);
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  if (base_platform == HS_PLATFORM_PC_ENGINE) {
    gboolean was_sgx = Mednafen::MDFN_GetSettingB ("pce_fast.forcesgx");

    if (self->pce_use_sgx != was_sgx) {
      GError *error = NULL;

      Mednafen::MDFNI_SetSetting ("pce_fast.forcesgx", self->pce_use_sgx ? "1" : "0");

      if (!reload_game (self, &error)) {
        callback (core, &error);
        return;
      }
    }
  }

  if (!Mednafen::MDFNI_LoadState (path, "")) {
    GError *error = NULL;
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load state");
    callback (core, &error);
    return;
  }

  self->colorburst_offset = hs_core_get_colorburst_offset (core);

  if (base_platform == HS_PLATFORM_PC_ENGINE)
    self->colorburst_offset = (self->colorburst_offset > 0.5) ? 0.5 : 0;

  callback (core, NULL);
}

static void
mednafen_core_save_state (HsCore          *core,
                          const char      *path,
                          HsStateCallback  callback)
{
  if (!Mednafen::MDFNI_SaveState (path, "", NULL, NULL, NULL)) {
    GError *error = NULL;
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to save state");
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static double
mednafen_core_get_frame_rate (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  return self->game->fps / 65536.0 / 256.0;
}

static double
mednafen_core_get_aspect_ratio (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  if (self->game == NULL)
    return 1;

  return self->game->nominal_width / (double) self->game->nominal_height;
}

static double
mednafen_core_get_sample_rate (HsCore *core)
{
  return SAMPLE_RATE;
}

static int
mednafen_core_get_channels (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  return self->game->soundchan;
}

static HsRegion
mednafen_core_get_region (HsCore *core)
{
  HsPlatform platform = hs_core_get_platform (core);
  HsPlatform base_platform = hs_platform_get_base_platform (platform);

  switch (base_platform) {
  case HS_PLATFORM_ATARI_LYNX:
  case HS_PLATFORM_NEO_GEO_POCKET:
  case HS_PLATFORM_VIRTUAL_BOY:
  case HS_PLATFORM_WONDERSWAN:
    return HS_REGION_UNKNOWN;


  case HS_PLATFORM_PC_ENGINE:
  case HS_PLATFORM_PLAYSTATION:
  case HS_PLATFORM_SEGA_SATURN:
    if (mednafen_core_get_frame_rate (core) > 55)
      return HS_REGION_NTSC;
    else
      return HS_REGION_PAL;

  default:
    g_assert_not_reached ();
  }
}

static guint
mednafen_core_get_media_length (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  return self->media_length;
}

static guint
mednafen_core_get_current_media (HsCore *core)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  return self->current_disc;
}

static void
set_media_cb (gpointer data)
{
  guint media = GPOINTER_TO_UINT (data);

  Mednafen::MDFNI_SetMedia (0, 2, media, 0);

  core->media_cb_id = 0;
}

static void
mednafen_core_set_current_media (HsCore *core, guint media)
{
  MednafenCore *self = MEDNAFEN_CORE (core);
  HsPlatform platform = hs_core_get_platform (core);

  if (platform != HS_PLATFORM_PC_ENGINE_CD &&
      platform != HS_PLATFORM_PLAYSTATION &&
      platform != HS_PLATFORM_SEGA_SATURN) {
    return;
  }

  g_clear_handle_id (&self->media_cb_id, g_source_remove);

  self->current_disc = media;
  hs_core_notify_current_media (core);

  Mednafen::MDFNI_SetMedia (0, 0, 0, 0);

  self->media_cb_id = g_timeout_add_once (1000, (GSourceOnceFunc) set_media_cb, GUINT_TO_POINTER (media));
}

static void
mednafen_core_finalize (GObject *object)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  g_clear_handle_id (&self->media_cb_id, g_source_remove);

  for (guint i = 0; i < 13; i++)
    g_free (self->input_buffer[i]);

  g_free (self->sound_buffer);

  core = NULL;

  G_OBJECT_CLASS (mednafen_core_parent_class)->finalize (object);
}

static void
mednafen_core_class_init (MednafenCoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  object_class->finalize = mednafen_core_finalize;

  core_class->load_rom = mednafen_core_load_rom;
  core_class->poll_input = mednafen_core_poll_input;
  core_class->run_frame = mednafen_core_run_frame;
  core_class->reset = mednafen_core_reset;
  core_class->stop = mednafen_core_stop;

  core_class->reload_save = mednafen_core_reload_save;
  core_class->sync_save = mednafen_core_sync_save;

  core_class->load_state = mednafen_core_load_state;
  core_class->save_state = mednafen_core_save_state;

  core_class->get_frame_rate = mednafen_core_get_frame_rate;
  core_class->get_aspect_ratio = mednafen_core_get_aspect_ratio;

  core_class->get_sample_rate = mednafen_core_get_sample_rate;
  core_class->get_channels = mednafen_core_get_channels;

  core_class->get_region = mednafen_core_get_region;

  core_class->get_media_length = mednafen_core_get_media_length;
  core_class->get_current_media = mednafen_core_get_current_media;
  core_class->set_current_media = mednafen_core_set_current_media;
}

static void
mednafen_core_init (MednafenCore *self)
{
  g_assert (!core);

  core = self;

  for (guint i = 0; i < 13; i++)
    self->input_buffer[i] = g_new0 (uint32_t, 9);

  self->sound_buffer = g_new0 (int16_t, SOUND_BUFFER_SIZE);
}

static void
mednafen_atari_lynx_core_init (HsAtariLynxCoreInterface *iface)
{
}

static void
mednafen_neo_geo_pocket_core_init (HsNeoGeoPocketCoreInterface *iface)
{
}

static void
mednafen_neo_geo_pocket_color_core_init (HsNeoGeoPocketColorCoreInterface *iface)
{
}

void
mednafen_pc_engine_core_set_enable_supergrafx (HsPcEngineCore *core, gboolean enable_supergrafx)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  self->pce_use_sgx = enable_supergrafx;
}

static void
mednafen_pc_engine_core_init (HsPcEngineCoreInterface *iface)
{
  iface->set_enable_supergrafx = mednafen_pc_engine_core_set_enable_supergrafx;
}

static void
mednafen_pc_engine_cd_core_init (HsPcEngineCdCoreInterface *iface)
{
}

HsPlayStationDualShockMode
mednafen_playstation_core_get_dualshock_mode (HsPlayStationCore *core, guint player)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  return self->psx_ds_analog[player] ? HS_PLAYSTATION_DUALSHOCK_ANALOG : HS_PLAYSTATION_DUALSHOCK_DIGITAL;
}

gboolean
mednafen_playstation_core_set_dualshock_mode (HsPlayStationCore *core, guint player, HsPlayStationDualShockMode mode)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  if ((*self->input_buffer[player] & PSX_LOCKED_STATUS_MASK) > 0)
    return FALSE;

  self->psx_ds_analog[player] = (mode == HS_PLAYSTATION_DUALSHOCK_ANALOG);
  return TRUE;
}

static void
mednafen_playstation_core_init (HsPlayStationCoreInterface *iface)
{
  iface->get_dualshock_mode = mednafen_playstation_core_get_dualshock_mode;
  iface->set_dualshock_mode = mednafen_playstation_core_set_dualshock_mode;
}

static void
mednafen_sega_saturn_core_set_controller (HsSegaSaturnCore *core, guint player, HsSegaSaturnController controller)
{
  MednafenCore *self = MEDNAFEN_CORE (core);

  self->ss_controller_type[player] = controller;

  switch (controller) {
  case HS_SEGA_SATURN_CONTROL_PAD:
    self->game->SetInput (player, "gamepad", (uint8_t *) self->input_buffer[player]);
    break;
  case HS_SEGA_SATURN_3D_CONTROL_PAD:
    self->game->SetInput (player, "3dpad", (uint8_t *) self->input_buffer[player]);
    break;
  default:
    g_assert_not_reached ();
  }
}

static void
mednafen_sega_saturn_core_init (HsSegaSaturnCoreInterface *iface)
{
  iface->set_controller = mednafen_sega_saturn_core_set_controller;
}

static void
mednafen_virtual_boy_core_init (HsVirtualBoyCoreInterface *iface)
{
}

static void
mednafen_wonderswan_core_init (HsWonderSwanCoreInterface *iface)
{
}

static void
mednafen_wonderswan_color_core_init (HsWonderSwanColorCoreInterface *iface)
{
}

GType
hs_get_core_type (void)
{
  return MEDNAFEN_TYPE_CORE;
}
