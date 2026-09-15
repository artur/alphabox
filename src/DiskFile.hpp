/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2020 Remy van Elst
 * Website: https://github.com/lenticularis39/axpbox
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
 * Contains definitions to use a file as a disk image.
 **/

#if !defined(__DISKFILE_H__)
#define __DISKFILE_H__

#include "Disk.hpp"
#include "DiskFileBinCue.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * \brief An opened, validated backing image: a flat raw/ISO file or a
 * BIN/CUE set (always read-only).
 *
 * Built completely by MediaImage::open() on the thread that requests the
 * media, so a bad path or CUE sheet never touches the drive's current media.
 * The destructor closes every handle.
 **/
struct MediaImage {
  std::string path;
  bool read_only = true;
  bool is_bincue = false;
  off_t_large byte_size = 0;    ///< Flat: file size. BIN/CUE: LBAs * 2048.
  FILE *handle = nullptr;       ///< Flat image handle.
  std::vector<CueTrack> tracks; ///< BIN/CUE tracks, each with its own handle.

  MediaImage() = default;
  MediaImage(const MediaImage &) = delete;
  MediaImage &operator=(const MediaImage &) = delete;
  ~MediaImage();

  /// Open and validate \a path; on failure returns nullptr and sets \a error.
  static std::unique_ptr<MediaImage> open(const std::string &path,
                                          bool read_only, std::string &error,
                                          const char *devid);
};

/// Outcome of an operator media request.
struct MediaResult {
  bool ok;
  std::string message;
  bool locked = false; ///< Refused because the guest locked the drive.
};

/// Snapshot of a removable drive, safe to take from any thread.
struct MediaStatus {
  std::string devid;
  bool cdrom = false;
  bool floppy = false;
  bool present = false;   ///< Media loaded (as of the last safe point).
  bool tray_open = false; ///< Opened by the guest (START STOP UNIT).
  bool locked = false;    ///< Guest PREVENT MEDIUM REMOVAL.
  bool read_only = false;
  bool request_pending = false; ///< An operator request awaits a safe point.
  std::string path;             ///< Loaded image ("" when empty).
};

/**
 * \brief Emulated disk that uses an image file.
 *
 * Supports raw/ISO images and BIN/CUE images (a file name ending in .cue,
 * case-insensitive). CD-ROM and floppy drives are removable: they may start
 * empty and accept operator insert/eject requests from any thread through
 * request_insert() / request_eject(). A request opens the new image on the
 * calling thread and parks it in a one-slot pending request (latest wins);
 * the drive swaps it in only at a safe point on the side that owns guest
 * I/O:
 *   - SCSI / ATAPI: at selection (scsi_select_me) or, from check_state, while
 *     the SCSI bus is free. Both hold media_mutex, so selection cannot start
 *     in the middle of a swap.
 *   - Floppy: by the FDC (service_media_request) under its controller mutex,
 *     when no command is in progress.
 **/
class CDiskFile : public CDisk {
public:
  CDiskFile(CConfigurator *cfg, CSystem *sys, CDiskController *c, int idebus,
            int idedev);
  virtual ~CDiskFile(void);

  virtual bool seek_byte(off_t_large byte);
  virtual size_t read_bytes(void *dest, size_t bytes);
  virtual size_t write_bytes(void *src, size_t bytes);
  virtual void flush();

  virtual void scsi_select_me(int bus);
  virtual void check_state();
  virtual void start_threads();
  virtual void stop_threads();
  virtual MediaRelease service_media_request();

  // ---------------------------------------------------------------
  // Operator media control (any thread)
  // ---------------------------------------------------------------
  /// Open \a path now; if valid, queue it to replace the current media.
  /// Refused while the guest has the drive locked, unless \a force.
  MediaResult request_insert(const std::string &path, bool read_only,
                             bool force = false);
  /// Queue removal of the current media (same lock rule).
  MediaResult request_eject(bool force = false);
  MediaStatus status();

  bool is_floppy() const { return floppy; }
  bool configured_read_only() const { return cfg_read_only; }

  // ---------------------------------------------------------------
  // BIN/CUE query interface (guest I/O thread; sane defaults for flat
  // images and empty drives).
  // ---------------------------------------------------------------
  bool is_bincue_image() const { return image && image->is_bincue; }
  int get_track_count() const {
    return is_bincue_image() ? (int)image->tracks.size() : 0;
  }
  const CueTrack *get_track(int idx) const {
    if (idx < 0 || idx >= get_track_count())
      return nullptr;
    return &image->tracks[idx];
  }
  const CueTrack *get_track_for_lba(long lba) const;

protected:
  virtual void guest_tray(bool load);

private:
  enum class MediaOp { None, Insert, Eject };

  std::unique_ptr<MediaImage> apply_pending_locked();
  std::unique_ptr<MediaImage>
  install_locked(std::unique_ptr<MediaImage> incoming);
  void swap_stress_loop(std::string first, std::string second, long ms);

  /// Current media. Read without locking by the thread owning guest I/O;
  /// only ever replaced under media_mutex at a safe point.
  std::unique_ptr<MediaImage> image;

  std::mutex media_mutex; ///< Guards the pending slot, image replacement,
                          ///< the tray fields and selection.
  MediaOp pending_op = MediaOp::None;
  std::unique_ptr<MediaImage> pending_image;
  bool pending_force = false;
  std::atomic<bool> pending{false}; ///< Cheap "is anything queued" check.

  std::string tray_path;      ///< Media removed by a guest tray open.
  bool tray_read_only = true; ///< Its read-only flag.
  bool tray_open = false;     ///< Mirror of MEDIA_TRAY_OPEN for status().

  bool floppy = false;
  bool cfg_read_only = false;
  bool allow_guest_eject = true;
  std::string defaultFilename;

  // AXPBOX_MEDIA_SWAP stress hook
  std::unique_ptr<std::thread> swap_thread;
  std::mutex swap_wait_mutex;
  std::condition_variable swap_wait;
  bool swap_stop = false;
};

/**
 * \brief Registry of removable image drives (CD-ROM and floppy CDiskFiles).
 *
 * Neutral entry point for front ends: the disk layer does not know about any
 * GUI. Drives register on construction and unregister on destruction; they
 * live for the whole emulator run.
 **/
class RemovableMedia {
public:
  /// Pointer snapshots: only valid while the system is running. Front ends
  /// that may outlive the drives (e.g. dialog callbacks at exit) should use
  /// insert_into_first_cdrom() instead.
  static std::vector<CDiskFile *> list();
  static CDiskFile *first_cdrom();
  /// request_insert() on the first CD-ROM drive, resolved and called under
  /// the registry lock so a drive being destroyed is never touched.
  static MediaResult insert_into_first_cdrom(const std::string &path,
                                             bool read_only, bool force);
  /// Create a new zero-filled image of \a bytes bytes (refuses to overwrite).
  static MediaResult create_blank_image(const std::string &path,
                                        off_t_large bytes);

private:
  friend class CDiskFile;
  static void add(CDiskFile *drive);
  static void remove(CDiskFile *drive);
};

#endif // !defined(__DISKFILE_H__)
