/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2020 Remy van Elst
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/**
 * \file
 * Contains code to use a file as a disk image, including removable media
 * (operator insert/eject, guest tray) for CD-ROM and floppy drives.
 **/

#include "DiskFile.hpp"
#include "StdAfx.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>

// ===========================================================================
//  Path / CUE helpers
// ===========================================================================

static char path_separator() {
#if defined(_WIN32)
  return '\\';
#elif defined(__VMS)
  return ']';
#else
  return '/';
#endif
}

/// True when \a path ends with ".cue" (case-insensitive).
static bool has_cue_extension(const std::string &path) {
  if (path.size() < 4)
    return false;
  const char *ext = path.c_str() + path.size() - 4;
  return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'c' &&
         tolower((unsigned char)ext[2]) == 'u' &&
         tolower((unsigned char)ext[3]) == 'e';
}

static long msf_to_lba(int m, int s, int f) {
  return (long)(m * 60 + s) * 75 + f;
}

/**
 * \brief Parse a cue track-mode string and fill in the sector geometry.
 *
 * AUDIO        : 2352 / offset 0  / data 2352  (raw PCM)
 * MODE1/2048   : 2048 / offset 0  / data 2048  (cooked)
 * MODE1/2352   : 2352 / offset 16 / data 2048
 * MODE2/2336   : 2336 / offset 8  / data 2048
 * MODE2/2352   : 2352 / offset 24 / data 2048
 * Unknown modes fall back to MODE1/2352 with a warning.
 **/
static CueTrackMode parse_mode_string(const char *s, CueTrack &trk) {
  struct ModeInfo {
    const char *name;
    CueTrackMode mode;
    int sector_size;
    int data_offset;
    int data_size;
  };
  static const ModeInfo modes[] = {
      {"AUDIO", TRACK_MODE_AUDIO, 2352, 0, 2352},
      {"MODE1/2048", TRACK_MODE1_2048, 2048, 0, 2048},
      {"MODE1/2352", TRACK_MODE1_2352, 2352, 16, 2048},
      {"MODE2/2336", TRACK_MODE2_2336, 2336, 8, 2048},
      {"MODE2/2352", TRACK_MODE2_2352, 2352, 24, 2048},
  };
  const ModeInfo *m = &modes[2];
  bool known = false;
  for (const ModeInfo &candidate : modes) {
    if (strcmp(s, candidate.name) == 0) {
      m = &candidate;
      known = true;
      break;
    }
  }
  if (!known)
    printf("CDiskFile: Unknown track mode '%s', defaulting to MODE1/2352\n", s);
  trk.sectorSize = m->sector_size;
  trk.dataOffset = m->data_offset;
  trk.dataSize = m->data_size;
  return m->mode;
}

/**
 * \brief Parse a .cue sheet into \a tracks and open every .bin file.
 *
 * .bin names are resolved relative to the directory of the .cue file. On
 * failure returns false with \a error set; handles already opened stay in
 * \a tracks and are closed by the owning MediaImage.
 **/
static bool parse_cue(const std::string &cue_path,
                      std::vector<CueTrack> &tracks, std::string &error,
                      const char *devid) {
  FILE *f = fopen(cue_path.c_str(), "r");
  if (!f) {
    error = "cannot open " + cue_path + ": " + strerror(errno);
    return false;
  }

  std::string cue_dir;
  size_t last_sep = cue_path.find_last_of(path_separator());
  if (last_sep != std::string::npos)
    cue_dir = cue_path.substr(0, last_sep);

  char line[512];
  char current_bin[BINCUE_MAX_PATH] = "";

  while (fgets(line, (int)sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' ||
                       end[-1] == '\t'))
      --end;
    *end = '\0';
    if (*p == '\0' || *p == ';' || *p == '#')
      continue;

    if (strncmp(p, "FILE ", 5) == 0) {
      // FILE "name.bin" BINARY   (some tools omit the quotes)
      std::string name;
      char *q1 = strchr(p, '"');
      char *q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
      if (q1 && q2) {
        name.assign(q1 + 1, q2);
      } else {
        char raw[BINCUE_MAX_PATH] = "";
        sscanf(p + 5, "%511s", raw);
        name = raw;
      }

#if defined(_WIN32)
      bool is_absolute = !name.empty() && (name[0] == '\\' ||
                                           (name.size() > 1 && name[1] == ':'));
#elif defined(__VMS)
      bool is_absolute = name.size() > 1 && name[1] == ':';
#else
      bool is_absolute = !name.empty() && name[0] == '/';
#endif
      if (!is_absolute && !cue_dir.empty())
        name = cue_dir + path_separator() + name;
      if (name.size() >= BINCUE_MAX_PATH) {
        fclose(f);
        error = "BIN file path too long in " + cue_path;
        return false;
      }
      strcpy(current_bin, name.c_str());
    } else if (strncmp(p, "TRACK ", 6) == 0) {
      CueTrack trk;
      memset(&trk, 0, sizeof(trk));
      char mode_str[32] = "";
      sscanf(p + 6, "%d %31s", &trk.number, mode_str);
      trk.mode = parse_mode_string(mode_str, trk);
      strcpy(trk.filename, current_bin);
      tracks.push_back(trk);
      printf("%s:   Track %02d  %s  sectorSize=%d  dataOffset=%d\n", devid,
             trk.number, mode_str, trk.sectorSize, trk.dataOffset);
    } else if (strncmp(p, "INDEX ", 6) == 0) {
      if (tracks.empty())
        continue;
      int index_num = 0, m = 0, s = 0, fr = 0;
      if (sscanf(p + 6, "%d %d:%d:%d", &index_num, &m, &s, &fr) == 4) {
        if (index_num == 0)
          tracks.back().pregapLBA = msf_to_lba(m, s, fr);
        else if (index_num == 1)
          tracks.back().startLBA = msf_to_lba(m, s, fr);
      }
    } else if (strncmp(p, "PREGAP ", 7) == 0) {
      if (tracks.empty())
        continue;
      int m = 0, s = 0, fr = 0;
      if (sscanf(p + 7, "%d:%d:%d", &m, &s, &fr) == 3)
        tracks.back().pregapLBA = msf_to_lba(m, s, fr);
    }
    // Other keywords (TITLE, PERFORMER, ...) are ignored.
  }
  fclose(f);

  if (tracks.empty()) {
    error = cue_path + " contains no TRACK entries";
    return false;
  }

  for (size_t i = 0; i + 1 < tracks.size(); i++)
    tracks[i].endLBA = tracks[i + 1].startLBA;

  // Single-bin layout: every track lives in one file at startLBA*sectorSize.
  // Multi-bin layout: each track's data starts at byte 0 of its own file.
  bool single_bin = true;
  for (size_t i = 1; i < tracks.size(); i++)
    if (strcmp(tracks[0].filename, tracks[i].filename) != 0)
      single_bin = false;

  for (size_t i = 0; i < tracks.size(); i++) {
    CueTrack &trk = tracks[i];
    if (!trk.filename[0]) {
      error = "track without a FILE entry in " + cue_path;
      return false;
    }
    trk.fileHandle = fopen_large(trk.filename, "rb");
    if (!trk.fileHandle) {
      error = std::string("cannot open BIN file ") + trk.filename + ": " +
              strerror(errno);
      return false;
    }
    trk.fileOffset =
        single_bin ? (off_t_large)trk.startLBA * trk.sectorSize : 0;

    if (i == tracks.size() - 1 && trk.endLBA == 0) {
      fseek_large(trk.fileHandle, 0, SEEK_END);
      off_t_large bin_bytes = ftell_large(trk.fileHandle);
      if (single_bin)
        bin_bytes -= (off_t_large)trk.startLBA * trk.sectorSize;
      trk.endLBA = trk.startLBA + (long)(bin_bytes / trk.sectorSize);
    }

    printf("%s:   Track %02d  LBA %ld..%ld  file=%s  offset=%" PRId64 "\n",
           devid, trk.number, trk.startLBA, trk.endLBA, trk.filename,
           trk.fileOffset);
  }
  return true;
}

/// Translate a logical LBA into a track index and the byte offset of its raw
/// sector in that track's file.
static bool lba_to_file_position(const MediaImage &img, long lba,
                                 int &track_idx, off_t_large &file_offset) {
  for (size_t i = 0; i < img.tracks.size(); i++) {
    const CueTrack &trk = img.tracks[i];
    if (lba >= trk.startLBA && lba < trk.endLBA) {
      track_idx = (int)i;
      file_offset =
          trk.fileOffset + (off_t_large)(lba - trk.startLBA) * trk.sectorSize;
      return true;
    }
  }
  return false;
}

// ===========================================================================
//  MediaImage
// ===========================================================================

MediaImage::~MediaImage() {
  if (handle)
    fclose(handle);
  for (CueTrack &trk : tracks)
    if (trk.fileHandle)
      fclose(trk.fileHandle);
}

void MediaImageDeleter::operator()(MediaImage *image) const { delete image; }

std::unique_ptr<MediaImage> MediaImage::open(const std::string &path,
                                             bool read_only, std::string &error,
                                             const char *devid) {
  if (path.empty()) {
    error = "no image file name given";
    return nullptr;
  }

  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    error = "cannot open " + path + ": " + strerror(errno);
    return nullptr;
  }
  if ((st.st_mode & S_IFMT) == S_IFDIR) {
    error = path + " is a directory";
    return nullptr;
  }

  std::unique_ptr<MediaImage> img(new MediaImage());
  img->path = path;

  if (has_cue_extension(path)) {
    img->is_bincue = true;
    img->read_only = true; // BIN/CUE is always read-only
    if (!parse_cue(path, img->tracks, error, devid))
      return nullptr;
    for (const CueTrack &trk : img->tracks)
      if (trk.endLBA > trk.startLBA)
        img->byte_size += (off_t_large)(trk.endLBA - trk.startLBA) * 2048;
    if (img->byte_size <= 0) {
      error = path + " describes no data sectors";
      return nullptr;
    }
    return img;
  }

  img->read_only = read_only;
  img->handle = fopen_large(path.c_str(), read_only ? "rb" : "rb+");
  if (!img->handle) {
    error = "cannot open " + path + (read_only ? "" : " for writing") + ": " +
            strerror(errno);
    return nullptr;
  }
  if (fseek_large(img->handle, 0, SEEK_END) != 0) {
    error = "cannot determine the size of " + path;
    return nullptr;
  }
  img->byte_size = ftell_large(img->handle);
  if (img->byte_size <= 0) {
    error = path + " is empty";
    return nullptr;
  }
  fseek_large(img->handle, 0, SEEK_SET);
  return img;
}

// ===========================================================================
//  RemovableMedia registry
// ===========================================================================

static std::mutex &registry_mutex() {
  static std::mutex m;
  return m;
}

static std::vector<CDiskFile *> &registry() {
  static std::vector<CDiskFile *> drives;
  return drives;
}

void RemovableMedia::add(CDiskFile *drive) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().push_back(drive);
}

void RemovableMedia::remove(CDiskFile *drive) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto &drives = registry();
  drives.erase(std::remove(drives.begin(), drives.end(), drive), drives.end());
}

std::vector<CDiskFile *> RemovableMedia::list() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  return registry();
}

CDiskFile *RemovableMedia::first_cdrom() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  for (CDiskFile *drive : registry())
    if (drive->cdrom())
      return drive;
  return nullptr;
}

MediaResult RemovableMedia::insert_into_first_cdrom(const std::string &path,
                                                    bool read_only,
                                                    bool force) {
  // Holding the registry lock across the request keeps ~CDiskFile (which
  // unregisters first) from destroying the drive underneath us.
  std::lock_guard<std::mutex> lock(registry_mutex());
  for (CDiskFile *drive : registry())
    if (drive->cdrom())
      return drive->request_insert(path, read_only, force);
  return {false, "no CD-ROM image drive is configured"};
}

MediaResult RemovableMedia::create_blank_image(const std::string &path,
                                               off_t_large bytes) {
  if (path.empty() || bytes <= 0)
    return {false, "a file name and a positive size are required"};

  FILE *f = fopen(path.c_str(), "rb");
  if (f) {
    fclose(f);
    return {false, path + " already exists"};
  }

  f = fopen_large(path.c_str(), "wb");
  if (!f)
    return {false, "cannot create " + path + ": " + strerror(errno)};

  std::vector<char> zeros(1024 * 1024, 0);
  off_t_large left = bytes;
  bool ok = true;
  while (left > 0 && ok) {
    size_t chunk = (size_t)std::min<off_t_large>(left, zeros.size());
    ok = fwrite(zeros.data(), 1, chunk, f) == chunk;
    left -= chunk;
  }
  if (fclose(f) != 0)
    ok = false;
  if (!ok) {
    ::remove(path.c_str());
    return {false, "cannot write " + path + ": " + strerror(errno)};
  }
  return {true, "created " + path + " (" + std::to_string((long long)bytes) +
                    " bytes)"};
}

// ===========================================================================
//  CDiskFile: construction
// ===========================================================================

static char default_cd_model[] = "CD-ROM";
static char default_floppy_model[] = "FLOPPY";

CDiskFile::CDiskFile(CConfigurator *cfg, CSystem *sys, CDiskController *c,
                     int idebus, int idedev)
    : CDisk(cfg, sys, c, idebus, idedev) {
  const char *parent_type = myCfg->get_myParent()->get_myValue();
  floppy = parent_type && strcmp(parent_type, "floppy") == 0;
  is_removable = is_cdrom || floppy;
  cfg_read_only = read_only;
  allow_guest_eject = myCfg->get_bool_value("allow_guest_eject", true);

  char *file = myCfg->get_text_value("file");
  bool have_file = file && *file;
  std::string path;
  if (have_file) {
    path = file;
  } else if (!is_removable) {
    // Hard disks fall back to a default image name, created through
    // autocreate_size when it does not exist yet.
    defaultFilename = std::string(devid_string) + ".default.img";
    fprintf(stderr, "%s: Disk has no filename attached! Assuming default: %s\n",
            devid_string, defaultFilename.c_str());
    path = defaultFilename;
  }

  std::unique_ptr<MediaImage> img;
  if (!path.empty()) {
    std::string error;
    img = MediaImage::open(path, read_only, error, devid_string);

    u64 autocreate_mb =
        myCfg->get_num_value("autocreate_size", false, 0) / 1024 / 1024;
    if (!img && !is_cdrom && autocreate_mb > 0) {
      struct stat st;
      if (stat(path.c_str(), &st) != 0 && errno == ENOENT) {
        MediaResult r = RemovableMedia::create_blank_image(
            path, (off_t_large)autocreate_mb * 1024 * 1024);
        printf("%s: %s\n", devid_string, r.message.c_str());
        if (r.ok)
          img = MediaImage::open(path, read_only, error, devid_string);
      }
    }

    if (!img) {
      if (!is_removable)
        FAILURE_2(Runtime, "%s: %s", devid_string, error.c_str());
      printf("%s: %s; the drive starts empty.\n", devid_string, error.c_str());
    }
  }

  {
    std::lock_guard<std::mutex> lock(media_mutex);
    install_locked(std::move(img));
  }
  state.scsi.media_changed = 0;

  char *model_default =
      !path.empty() ? (have_file ? file : &defaultFilename[0])
                    : (is_cdrom ? default_cd_model : default_floppy_model);
  model_number = myCfg->get_text_value("model_number", model_default);

  // Advance model_number pointer past any directory component.
  char sep = path_separator();
  for (char *p = model_number; *p; p++)
    if (*p == sep)
      model_number = p + 1;

  if (image)
    printf("%s: Mounted file %s, %" PRId64 " %zu-byte blocks, "
           "%" PRId64 "/%ld/%ld.\n",
           devid_string, image->path.c_str(), byte_size / state.block_size,
           state.block_size, cylinders, heads, sectors);
  else
    printf("%s: Removable drive, no media loaded.\n", devid_string);

  if (is_removable)
    RemovableMedia::add(this);
}

CDiskFile::~CDiskFile(void) {
  stop_threads();
  if (is_removable)
    RemovableMedia::remove(this);
  printf("%s: Closing file.\n", devid_string);
  // image and pending_image close their handles.
}

// ===========================================================================
//  Media swapping
// ===========================================================================

/**
 * Replace the current media with \a incoming (may be null = empty) and
 * return the old image, which the caller must release after unlocking.
 * Caller holds media_mutex and is at a safe point.
 **/
std::unique_ptr<MediaImage>
CDiskFile::install_locked(std::unique_ptr<MediaImage> incoming) {
  std::unique_ptr<MediaImage> old = std::move(image);
  image = std::move(incoming);
  byte_size = image ? image->byte_size : 0;
  read_only = image ? image->read_only : cfg_read_only;
  state.byte_pos = 0;
  determine_layout();
  return old;
}

/**
 * Apply the pending operator request. Caller holds media_mutex and is at a
 * safe point. Returns the image to release after unlocking (the old media,
 * or a refused incoming one).
 **/
std::unique_ptr<MediaImage> CDiskFile::apply_pending_locked() {
  pending.store(false);
  MediaOp op = pending_op;
  pending_op = MediaOp::None;
  std::unique_ptr<MediaImage> incoming = std::move(pending_image);
  if (op == MediaOp::None)
    return nullptr;

  // The guest may have locked the drive after the request was accepted.
  if (state.scsi.locked && !pending_force) {
    printf("%s: Media %s refused: the guest has locked the drive.\n",
           devid_string, op == MediaOp::Insert ? "insert" : "eject");
    return incoming;
  }

  std::unique_ptr<MediaImage> old = install_locked(std::move(incoming));
  tray_path.clear();
  tray_open = false;
  int flags = state.scsi.media_changed & ~MEDIA_TRAY_OPEN;
  if (op == MediaOp::Insert)
    flags |= MEDIA_UNIT_ATTENTION | MEDIA_CHANGE_LINE | MEDIA_GESN_EVENT;
  else
    flags =
        (flags & ~MEDIA_UNIT_ATTENTION) | MEDIA_CHANGE_LINE | MEDIA_GESN_EVENT;
  state.scsi.media_changed = flags;

  if (image)
    printf("%s: Media changed to %s (%" PRId64 " bytes%s).\n", devid_string,
           image->path.c_str(), image->byte_size,
           image->read_only ? ", read-only" : "");
  else
    printf("%s: Media ejected.\n", devid_string);
  return old;
}

MediaRelease CDiskFile::service_media_request() {
  if (!pending.load())
    return MediaRelease();
  std::lock_guard<std::mutex> lock(media_mutex);
  return MediaRelease(apply_pending_locked().release());
}

void CDiskFile::scsi_select_me(int bus) {
  if (!is_removable) {
    CDisk::scsi_select_me(bus);
    return;
  }

  // Selection starts a new command: swap here, and keep the bus phase change
  // inside the lock so check_state never sees a stale "bus free".
  std::unique_ptr<MediaImage> release;
  {
    std::lock_guard<std::mutex> lock(media_mutex);
    if (pending.load())
      release = apply_pending_locked();
    CDisk::scsi_select_me(bus);
  }
}

void CDiskFile::check_state() {
  // Floppies are serviced by the FDC; drives not on a SCSI/ATAPI bus wait
  // for their controller.
  if (!is_removable || !scsi_bus[0] || !pending.load())
    return;

  std::unique_ptr<MediaImage> release;
  {
    std::lock_guard<std::mutex> lock(media_mutex);
    if (pending.load() && scsi_bus[0]->get_phase() == SCSI_PHASE_FREE)
      release = apply_pending_locked();
  }
}

MediaResult CDiskFile::request_insert(const std::string &path, bool ro,
                                      bool force) {
  if (!is_removable)
    return {false, std::string(devid_string) + " is not a removable drive"};
  if (media_locked() && !force)
    return {false,
            std::string(devid_string) +
                " is locked by the guest (PREVENT MEDIUM REMOVAL)",
            true};

  std::string error;
  std::unique_ptr<MediaImage> img =
      MediaImage::open(path, ro, error, devid_string);
  if (!img)
    return {false, error};

  std::unique_ptr<MediaImage> superseded;
  {
    std::lock_guard<std::mutex> lock(media_mutex);
    superseded = std::move(pending_image);
    pending_image = std::move(img);
    pending_op = MediaOp::Insert;
    pending_force = force;
    pending.store(true);
  }
  return {true, std::string(devid_string) + ": insert of " + path + " queued"};
}

MediaResult CDiskFile::request_eject(bool force) {
  if (!is_removable)
    return {false, std::string(devid_string) + " is not a removable drive"};
  if (media_locked() && !force)
    return {false,
            std::string(devid_string) +
                " is locked by the guest (PREVENT MEDIUM REMOVAL)",
            true};

  std::unique_ptr<MediaImage> superseded;
  {
    std::lock_guard<std::mutex> lock(media_mutex);
    superseded = std::move(pending_image);
    pending_op = MediaOp::Eject;
    pending_force = force;
    pending.store(true);
  }
  return {true, std::string(devid_string) + ": eject queued"};
}

MediaStatus CDiskFile::status() {
  MediaStatus s;
  s.devid = devid_string;
  s.cdrom = is_cdrom;
  s.floppy = floppy;
  s.locked = media_locked();
  std::lock_guard<std::mutex> lock(media_mutex);
  s.present = image != nullptr;
  s.path = image ? image->path : std::string();
  s.read_only = image ? image->read_only : cfg_read_only;
  s.tray_open = tray_open;
  s.request_pending = pending_op != MediaOp::None;
  return s;
}

/**
 * Guest START STOP UNIT LoEj=1: open (load=false) or close (load=true) the
 * tray. Runs on the guest I/O thread inside the command.
 **/
void CDiskFile::guest_tray(bool load) {
  if (!allow_guest_eject) {
    printf("%s: Guest %s request ignored (allow_guest_eject = false).\n",
           devid_string, load ? "load" : "eject");
    return;
  }

  std::unique_ptr<MediaImage> release;
  if (!load) {
    std::lock_guard<std::mutex> lock(media_mutex);
    if (image) {
      tray_path = image->path;
      tray_read_only = image->read_only;
    }
    release = install_locked(nullptr);
    tray_open = true;
    state.scsi.media_changed =
        (state.scsi.media_changed & ~MEDIA_UNIT_ATTENTION) | MEDIA_TRAY_OPEN |
        MEDIA_CHANGE_LINE | MEDIA_GESN_EVENT;
    printf("%s: Guest opened the tray%s%s.\n", devid_string,
           tray_path.empty() ? "" : ", removed ", tray_path.c_str());
    return;
  }

  std::string path;
  bool ro = true;
  {
    std::lock_guard<std::mutex> lock(media_mutex);
    if (!tray_open) {
      // Already closed; also drop a stray tray-open flag (e.g. restored).
      state.scsi.media_changed &= ~MEDIA_TRAY_OPEN;
      return;
    }
    path = tray_path;
    ro = tray_read_only;
  }

  // Re-open the remembered image outside the lock (it may be slow).
  std::unique_ptr<MediaImage> img;
  if (!path.empty()) {
    std::string error;
    img = MediaImage::open(path, ro, error, devid_string);
    if (!img)
      printf("%s: Guest closed the tray, but %s; the drive stays empty.\n",
             devid_string, error.c_str());
  }

  std::lock_guard<std::mutex> lock(media_mutex);
  tray_open = false;
  tray_path.clear();
  state.scsi.media_changed &= ~MEDIA_TRAY_OPEN;
  if (img) {
    release = install_locked(std::move(img));
    state.scsi.media_changed |=
        MEDIA_UNIT_ATTENTION | MEDIA_CHANGE_LINE | MEDIA_GESN_EVENT;
    printf("%s: Guest closed the tray, reloaded %s.\n", devid_string,
           path.c_str());
  }
}

// ===========================================================================
//  ALPHABOX_MEDIA_SWAP stress hook
// ===========================================================================

/**
 * ALPHABOX_MEDIA_SWAP=<image1>:<image2>:<ms> alternates two images on the first
 * CD-ROM drive every <ms> milliseconds (forced, ignoring the guest lock).
 * <ms> is the text after the last ':'. The first other ':' separates the two
 * images, except a Windows drive-letter colon: a single letter that starts a
 * path (at the start of the value or right after a separator ':').
 **/
void CDiskFile::start_threads() {
  const char *spec = getenv("ALPHABOX_MEDIA_SWAP");
  if (!spec || !*spec || swap_thread || RemovableMedia::first_cdrom() != this)
    return;

  std::string s(spec);
  size_t last = s.rfind(':');
  size_t mid = std::string::npos;
  long ms = 0;
  if (last != std::string::npos) {
    for (size_t i = 0; i < last; i++) {
      if (s[i] != ':')
        continue;
      bool drive_letter = i >= 1 && isalpha((unsigned char)s[i - 1]) &&
                          (i == 1 || s[i - 2] == ':');
      if (!drive_letter) {
        mid = i;
        break;
      }
    }
    char *end = nullptr;
    ms = strtol(s.c_str() + last + 1, &end, 10);
    if (end == s.c_str() + last + 1 || *end != '\0')
      ms = 0;
  }
  if (mid == std::string::npos || mid == 0 || mid + 1 >= last || ms <= 0) {
    printf("%s: ALPHABOX_MEDIA_SWAP ignored: expected <image1>:<image2>:<ms>\n",
           devid_string);
    return;
  }

  swap_stop = false;
  swap_thread = std::make_unique<std::thread>(
      [this, a = s.substr(0, mid), b = s.substr(mid + 1, last - mid - 1),
       ms]() { swap_stress_loop(a, b, ms); });
  printf(" %s(media swap)", devid_string);
}

void CDiskFile::stop_threads() {
  if (!swap_thread)
    return;
  {
    std::lock_guard<std::mutex> lock(swap_wait_mutex);
    swap_stop = true;
  }
  swap_wait.notify_all();
  swap_thread->join();
  swap_thread.reset();
}

void CDiskFile::swap_stress_loop(std::string first, std::string second,
                                 long ms) {
  unsigned long count = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(swap_wait_mutex);
      if (swap_wait.wait_for(lock, std::chrono::milliseconds(ms),
                             [this] { return swap_stop; }))
        return;
    }
    const std::string &path = (count % 2 == 0) ? second : first;
    MediaResult r = request_insert(path, true, true);
    count++;
    if (!r.ok || count <= 2 || count % 50 == 0)
      printf("%s: ALPHABOX_MEDIA_SWAP #%lu: %s\n", devid_string, count,
             r.message.c_str());
  }
}

// ===========================================================================
//  CDisk virtual interface overrides (guest I/O thread)
// ===========================================================================

/**
 * \brief Seek to an absolute logical byte position.
 *
 * Returns false (never throws) when the drive is empty or the position is
 * past the end of the media; callers turn that into a SCSI/FDC error.
 * For BIN/CUE images the position is logical (LBA * 2048) and the physical
 * seek happens in read_bytes().
 **/
bool CDiskFile::seek_byte(off_t_large byte) {
  MediaImage *img = image.get();
  if (!img || byte < 0 || byte >= byte_size)
    return false;

  if (!img->is_bincue && fseek_large(img->handle, byte, SEEK_SET) != 0)
    return false;
  state.byte_pos = byte;
  return true;
}

/**
 * \brief Read \a bytes bytes from the current position into \a dest.
 *
 * For BIN/CUE images: translates logical positions to physical file offsets,
 * extracting only user-data bytes and crossing track and file boundaries.
 * For audio tracks the raw sector is the payload.
 **/
size_t CDiskFile::read_bytes(void *dest, size_t bytes) {
  MediaImage *img = image.get();
  if (!img)
    return 0;

  if (!img->is_bincue) {
    size_t r = fread(dest, 1, bytes, img->handle);
    state.byte_pos += r;
    return r;
  }

  size_t total_read = 0;
  u8 *out = static_cast<u8 *>(dest);
  off_t_large pos = state.byte_pos;

  while (bytes > 0) {
    int track_idx = 0;
    off_t_large file_off = 0;
    if (!lba_to_file_position(*img, (long)(pos / 2048), track_idx, file_off))
      break; // beyond all tracks

    const CueTrack &trk = img->tracks[track_idx];
    long offset_in_sector = (long)(pos % 2048);
    size_t can_read = (size_t)(2048 - offset_in_sector);
    if (can_read > bytes)
      can_read = bytes;

    off_t_large phys = file_off + trk.dataOffset + offset_in_sector;
    if (fseek_large(trk.fileHandle, phys, SEEK_SET) != 0)
      break;
    size_t r = fread(out, 1, can_read, trk.fileHandle);
    if (r == 0)
      break;

    out += r;
    total_read += r;
    bytes -= r;
    pos += r;
  }

  state.byte_pos = pos;
  return total_read;
}

/**
 * \brief Write \a bytes bytes from \a src at the current position.
 *
 * Empty drives, read-only images and BIN/CUE images write nothing.
 **/
size_t CDiskFile::write_bytes(void *src, size_t bytes) {
  MediaImage *img = image.get();
  if (!img || img->read_only || img->is_bincue)
    return 0;

  size_t r = fwrite(src, 1, bytes, img->handle);
  state.byte_pos += r;
  return r;
}

void CDiskFile::flush() {
  MediaImage *img = image.get();
  if (img && img->handle && !img->read_only)
    fflush(img->handle);
}

const CueTrack *CDiskFile::get_track_for_lba(long lba) const {
  for (int i = 0; i < get_track_count(); i++) {
    const CueTrack &trk = image->tracks[i];
    if (lba >= trk.startLBA && lba < trk.endLBA)
      return &trk;
  }
  return nullptr;
}
