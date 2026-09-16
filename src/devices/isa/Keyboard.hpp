/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
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
 * Contains the definitions for the emulated Keyboard and mouse devices and
 *controller.
 **/
#if !defined(INCLUDED_KEYBOARD_H)
#define INCLUDED_KEYBOARD_H

#include "SystemComponent.hpp"
#include "gui/gui.hpp"

#define BX_KBD_ELEMENTS 16
#define BX_MOUSE_BUFF_SIZE 48

#define MOUSE_MODE_RESET 10
#define MOUSE_MODE_STREAM 11
#define MOUSE_MODE_REMOTE 12
#define MOUSE_MODE_WRAP 13

/**
 * \brief Emulated keyboard controller, keyboard and mouse.
 **/
class CKeyboard : public CSystemComponent {
public:
  CKeyboard(CConfigurator *cfg, CSystem *c);
  virtual ~CKeyboard();

  virtual void check_state();
  virtual void WriteMem(int index, u64 address, int dsize, u64 data);
  virtual u64 ReadMem(int index, u64 address, int dsize);
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);
  virtual void run();
  void execute();

  void gen_scancode(u32 key);
  void mouse_motion(int delta_x, int delta_y, int delta_z,
                    unsigned button_state);
  void set_mouse_capture(bool val);

  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();

private:
  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  bool StopThread;

  std::mutex kbdLock;

  u8 read_60();
  void write_60(u8 data);
  u8 read_64();
  void write_64(u8 data);
  void resetinternals(bool powerup);

  /// Origin of a byte in the controller queue.
  enum : u8 {
    KBC_SRC_CTRL = 0,     ///< controller reply, keyboard channel (IRQ1)
    KBC_SRC_CTRL_AUX = 1, ///< controller-generated aux byte (D3), IRQ12
    KBC_SRC_KBD = 2,      ///< keyboard device reply
    KBC_SRC_AUX = 3,      ///< mouse device reply
  };

  void kbd_enQ_sequence(const u8 *bytes, int count);
  void kbd_response_enQ(u8 data);
  void controller_enQ(u8 data, u8 source);
  bool controller_deQ();
  void load_output_buffer(u8 data, bool aux);
  void deliver_kbd_byte();
  void deliver_mouse_byte();
  bool kbd_input_held() const;
  bool mouse_input_held() const;
  bool kbd_tail_pending() const;
  bool mouse_tail_pending() const;
  void drop_unsent_mouse_packets();
  void discard_mouse_motion();
  void set_kbd_clock_enable(u8 value);
  void set_aux_clock_enable(u8 value);
  void ctrl_to_kbd(u8 value);
  void ctrl_to_mouse(u8 value);
  int build_mouse_packet(u8 *packet);
  bool mouse_enQ_packet(const u8 *packet, int bytes);
  void kbd_update_irq();
  void kbd_service();
  void create_mouse_packet();

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SKb_state {

    /// status bits matching the status port
    struct SAli_kbdc_status {
      bool pare; /**< Bit7, 1= parity error from keyboard/mouse - ignored. */
      bool tim;  /**< Bit6, 1= timeout from keyboard - ignored. */
      bool auxb; /**< Bit5, 1= mouse data waiting for CPU to read. */
      bool keyl; /**< Bit4, 1= keyswitch in lock position - ignored. */
      bool c_d;  /**< Bit3, 1=command to port 64h, 0=data to port 60h. */
      bool sysf; /**< Bit2 */
      bool inpb; /**< Bit1 */
      bool outb; /**< Bit0, 1= keyboard data or mouse data ready for CPU. Check
                    aux to see which. */
    } status;

    /* internal to our version of the keyboard controller */
    bool kbd_clock_enabled;
    bool aux_clock_enabled;
    bool allow_irq1;
    bool allow_irq12;
    u8 kbd_output_buffer;
    u8 aux_output_buffer;
    u8 last_kbd_byte; ///< last byte the keyboard sent (for FE resend)
    u8 last_aux_byte; ///< last byte the mouse sent (for FE resend)
    u8 last_comm;
    u8 expecting_port60h;
    u8 expecting_mouse_parameter;
    u8 last_mouse_command;
    bool scancodes_translate;
    bool expecting_scancodes_set;
    u8 current_scancodes_set;

    /// mouse status
    struct SAli_mouse {
      bool captured; // host mouse capture enabled

      //      u8   type;
      u8 sample_rate;
      u8 resolution_cpmm; // resolution in counts per mm
      u8 scaling;
      u8 mode;
      u8 saved_mode; // the mode prior to entering wrap mode
      bool enable;

      u8 get_status_byte() {

        // top bit is 0 , bit 6 is 1 if remote mode.
        u8 ret = (u8)((mode == MOUSE_MODE_REMOTE) ? 0x40 : 0);
        ret |= (enable << 5);
        ret |= (scaling == 1) ? 0 : (1 << 4);
        ret |= ((button_status & 0x1) << 2);
        ret |= ((button_status & 0x2) << 0);
        return ret;
      }

      u8 get_resolution_byte() {
        u8 ret = 0;

        switch (resolution_cpmm) {
        case 1:
          ret = 0;
          break;
        case 2:
          ret = 1;
          break;
        case 4:
          ret = 2;
          break;
        case 8:
          ret = 3;
          break;
        default:
          FAILURE(NotImplemented, "mouse: invalid resolution_cpmm");
        };
        return ret;
      }

      u8 button_status;
      s16 delayed_dx;
      s16 delayed_dy;
      s16 delayed_dz;
      u8 im_request;
      bool im_mode;
      bool data_pending;
      u8 reported_buttons; ///< button state carried by the last packet
      /// button states not yet packetized, oldest first (so a press and
      /// release between two packets both reach the guest)
#define BX_MOUSE_BUTTON_QSIZE 8
      u8 button_queue[BX_MOUSE_BUTTON_QSIZE];
      u8 button_queue_len;
    } mouse;

    /// internal keyboard buffer (unsolicited keystrokes only; command replies
    /// use the controller queue)
    struct SAli_kbdib {
      int num_elements;
      u8 buffer[BX_KBD_ELEMENTS];
      bool seq_start[BX_KBD_ELEMENTS]; ///< first byte of a key event
      int head;
      bool expecting_typematic;
      bool expecting_led_write;
      bool expecting_make_break;
      u8 delay;
      u8 repeat_rate;
      u8 led_status;
      bool scanning_enabled;
    } kbd_internal_buffer;

    /// internal mouse buffer (unsolicited stream packets only; command
    /// replies use the controller queue)
    struct SAli_mib {
      int num_elements;
      u8 buffer[BX_MOUSE_BUFF_SIZE];
      bool pkt_start[BX_MOUSE_BUFF_SIZE]; ///< first byte of a packet
      int head;
    } mouse_internal_buffer;

    /// Controller/response queue: controller replies and keyboard/mouse
    /// command replies, each byte tagged with its KBC_SRC_* origin.
#define BX_KBD_CONTROLLER_QSIZE 16
    u8 kbd_controller_Q[BX_KBD_CONTROLLER_QSIZE];
    u8 kbd_controller_Qsrc[BX_KBD_CONTROLLER_QSIZE];
    unsigned kbd_controller_Qsize;
  } state;
};

extern CKeyboard *theKeyboard;
#endif // !defined(INCLUDED_KEYBOARD_H)
