#define __BTSTACK_FILE__ "SwitchBluetooth.cpp"

#include "SwitchBluetooth.h"
#include "FastLogger.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <cstring>

#include "btstack.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

static uint8_t hid_service_buffer[700];
static uint8_t pnp_service_buffer[200];
static const char hid_device_name[] = "Wireless Gamepad";

void SwitchBluetooth::init() {
  _switchReport.batteryConnection = 0x80;
  
  // Initialize Bluetooth timing control
  _last_hid_report_time = 0;
  _pending_report_update = false;
  
  // Initialize command queue
  _queue_head = 0;
  _queue_tail = 0;
  _queue_full = false;
  _consolidation_active = false;
  
  bd_addr_t newAddr = {0x7c,
                       0xbb,
                       0x8a,
                       (uint8_t)(get_rand_32() % 0xff),
                       (uint8_t)(get_rand_32() % 0xff),
                       (uint8_t)(get_rand_32() % 0xff)};
  memcpy(_addr, newAddr, 6);
  
  if (cyw43_arch_init()) {
    return;
  }

  gap_discoverable_control(1);
  gap_set_class_of_device(0x2508);
  gap_set_local_name("Pro Controller");
  gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                       LM_LINK_POLICY_ENABLE_SNIFF_MODE);
  gap_set_allow_role_switch(true);

  hci_set_bd_addr(_addr);

  // L2CAP
  l2cap_init();
  sm_init();

  // HID Device - use simplified initialization
  hid_device_init(0, sizeof(switch_bt_report_descriptor),
                  switch_bt_report_descriptor);
}

// Bluetooth timing control for 125Hz HID rate (8ms minimum interval)
bool SwitchBluetooth::can_send_hid_report() {
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    return (current_time - _last_hid_report_time) >= 8; // 8ms = 125Hz
}

void SwitchBluetooth::mark_report_sent() {
    _last_hid_report_time = to_ms_since_boot(get_absolute_time());
    _pending_report_update = false;
}

void SwitchBluetooth::wait_for_hid_transmission() {
    // Non-blocking wait - only check if enough time has passed
    // Actual transmission timing is handled by the BTStack event system
    // This eliminates blocking sleep calls that interfere with macro timing
}

void SwitchBluetooth::start_consolidation() {
    _consolidation_active = true;
    _consolidation_start_time = to_ms_since_boot(get_absolute_time());
}

void SwitchBluetooth::end_consolidation() {
    _consolidation_active = false;
    // Process all queued commands into a single frame
    process_command_queue();
}

void SwitchBluetooth::queue_button_command(const char* button, bool pressed) {
    if (_queue_full) {
        FastLogger::log("Command queue full - dropping command");
        return;
    }
    
    // Add command to queue
    QueuedCommand& cmd = _command_queue[_queue_tail];
    cmd.type = pressed ? QueuedCommand::BUTTON_PRESS : QueuedCommand::BUTTON_RELEASE;
    strncpy(cmd.button_name, button, sizeof(cmd.button_name) - 1);
    cmd.button_name[sizeof(cmd.button_name) - 1] = '\0';
    cmd.pressed = pressed;
    
    _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
    if (_queue_tail == _queue_head) {
        _queue_full = true;
    }
    
    // If not consolidating, process immediately
    if (!_consolidation_active) {
        process_command_queue();
    }
}

void SwitchBluetooth::queue_stick_command(const char* stick, float h, float v) {
    if (_queue_full) {
        FastLogger::log("Command queue full - dropping stick command");
        return;
    }
    
    // Add stick command to queue
    QueuedCommand& cmd = _command_queue[_queue_tail];
    cmd.type = QueuedCommand::STICK_SET;
    strncpy(cmd.button_name, stick, sizeof(cmd.button_name) - 1);
    cmd.button_name[sizeof(cmd.button_name) - 1] = '\0';
    cmd.stick_h = h;
    cmd.stick_v = v;
    
    _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
    if (_queue_tail == _queue_head) {
        _queue_full = true;
    }
    
    // If not consolidating, process immediately
    if (!_consolidation_active) {
        process_command_queue();
    }
}

void SwitchBluetooth::process_command_queue() {
    bool state_changed = false;
    int commands_processed = 0;
    
    // Process all queued commands
    while (_queue_head != _queue_tail || _queue_full) {
        QueuedCommand& cmd = _command_queue[_queue_head];
        
        if (cmd.type == QueuedCommand::BUTTON_PRESS || cmd.type == QueuedCommand::BUTTON_RELEASE) {
            // Apply button command directly to report state
            bool pressed = (cmd.type == QueuedCommand::BUTTON_PRESS);
            set_button_direct(cmd.button_name, pressed);
            state_changed = true;
            commands_processed++;
        } else if (cmd.type == QueuedCommand::STICK_SET) {
            // Apply stick command directly to report state
            set_stick_direct(cmd.button_name, cmd.stick_h, cmd.stick_v);
            state_changed = true;
            commands_processed++;
        }
        
        _queue_head = (_queue_head + 1) % MAX_QUEUE_SIZE;
        _queue_full = false;
    }
    
    // Log consolidation for debugging
    if (commands_processed > 1) {
        FastLogger::log_fmt("Consolidated %d commands into single frame", commands_processed);
    }
    
    // Only mark for transmission if state actually changed
    if (state_changed) {
        _pending_report_update = true;
    }
}

bool SwitchBluetooth::has_queued_commands() {
    return (_queue_head != _queue_tail) || _queue_full;
}

// Optimized button control method with command queuing
void SwitchBluetooth::set_button(const char* button, bool pressed) {
    // Queue the command for frame consolidation
    queue_button_command(button, pressed);
}

// Direct button state modification (used by queue processor)
void SwitchBluetooth::set_button_direct(const char* button, bool pressed) {
    
    // Use a hash-like approach with first character for faster lookup
    switch (button[0]) {
        case 'a':
            if (button[1] == '\0') { // "a"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_A;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_A;
            }
            break;
        case 'b':
            if (button[1] == '\0') { // "b"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_B;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_B;
            }
            break;
        case 'x':
            if (button[1] == '\0') { // "x"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_X;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_X;
            }
            break;
        case 'y':
            if (button[1] == '\0') { // "y"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_Y;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_Y;
            }
            break;
        case 'l':
            if (button[1] == '\0') { // "l"
                if (pressed) _switchReport.buttons[2] |= SWITCH_MASK_L;
                else _switchReport.buttons[2] &= ~SWITCH_MASK_L;
            } else if (button[1] == '_' && button[2] == 's') { // "l_stick"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_L3;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_L3;
            }
            break;
        case 'r':
            if (button[1] == '\0') { // "r"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_R;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_R;
            } else if (button[1] == '_' && button[2] == 's') { // "r_stick"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_R3;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_R3;
            }
            break;
        case 'z':
            if (button[1] == 'l') { // "zl"
                if (pressed) _switchReport.buttons[2] |= SWITCH_MASK_ZL;
                else _switchReport.buttons[2] &= ~SWITCH_MASK_ZL;
            } else if (button[1] == 'r') { // "zr"
                if (pressed) _switchReport.buttons[0] |= SWITCH_MASK_ZR;
                else _switchReport.buttons[0] &= ~SWITCH_MASK_ZR;
            }
            break;
        case 'p':
            if (button[1] == 'l') { // "plus"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_PLUS;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_PLUS;
            }
            break;
        case 'm':
            if (button[1] == 'i') { // "minus"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_MINUS;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_MINUS;
            }
            break;
        case 'h':
            if (button[1] == 'o') { // "home"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_HOME;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_HOME;
            }
            break;
        case 'c':
            if (button[1] == 'a') { // "capture"
                if (pressed) _switchReport.buttons[1] |= SWITCH_MASK_CAPTURE;
                else _switchReport.buttons[1] &= ~SWITCH_MASK_CAPTURE;
            }
            break;
        case 'd':
            if (pressed) {
                if (button[5] == 'u') { // "dpad_up"
                    _switchReport.buttons[2] = (_switchReport.buttons[2] & 0xF0) | SWITCH_HAT_UP;
                } else if (button[5] == 'd') { // "dpad_down"
                    _switchReport.buttons[2] = (_switchReport.buttons[2] & 0xF0) | SWITCH_HAT_DOWN;
                } else if (button[5] == 'l') { // "dpad_left"
                    _switchReport.buttons[2] = (_switchReport.buttons[2] & 0xF0) | SWITCH_HAT_LEFT;
                } else if (button[5] == 'r') { // "dpad_right"
                    _switchReport.buttons[2] = (_switchReport.buttons[2] & 0xF0) | SWITCH_HAT_RIGHT;
                }
            } else {
                _switchReport.buttons[2] = (_switchReport.buttons[2] & 0xF0) | SWITCH_HAT_NOTHING;
            }
            break;
    }
}

void SwitchBluetooth::set_stick(const char* stick, float h, float v) {
    // Queue the command for frame consolidation
    queue_stick_command(stick, h, v);
}

// Direct stick state modification (used by queue processor)
void SwitchBluetooth::set_stick_direct(const char* stick, float h, float v) {
    
    // Convert from [-1.0, 1.0] to [0x000, 0xFFF] range
    uint16_t h_val = (uint16_t)((h + 1.0f) * 0x7FF);
    uint16_t v_val = (uint16_t)((v + 1.0f) * 0x7FF);
    
    // Clamp values
    if (h_val > 0xFFF) h_val = 0xFFF;
    if (v_val > 0xFFF) v_val = 0xFFF;
    
    // Optimized stick selection using first character
    if (stick[0] == 'l') { // "l_stick"
        _switchReport.l[0] = h_val & 0xFF;
        _switchReport.l[1] = ((h_val >> 8) & 0x0F) | ((v_val & 0x0F) << 4);
        _switchReport.l[2] = (v_val >> 4) & 0xFF;
    } else if (stick[0] == 'r') { // "r_stick"
        _switchReport.r[0] = h_val & 0xFF;
        _switchReport.r[1] = ((h_val >> 8) & 0x0F) | ((v_val & 0x0F) << 4);
        _switchReport.r[2] = (v_val >> 4) & 0xFF;
    }
}



// Implementation of SwitchCommon methods
void SwitchBluetooth::setSwitchRequestReport(uint8_t *report, int report_size) {
  memcpy(_switchRequestReport, report, report_size);
}

uint8_t *SwitchBluetooth::generate_report() {
  set_empty_report();
  _report[0] = 0xa1;
  switch (_switchRequestReport[10]) {
    case 0x01:  // BLUETOOTH_PAIR_REQUEST
      set_subcommand_reply();
      set_bt();
      break;
    case 0x02:  // REQUEST_DEVICE_INFO
      _device_info_queried = true;
      set_subcommand_reply();
      set_device_info();
      break;
    case 0x08:  // SET_SHIPMENT
      set_subcommand_reply();
      set_shipment();
      break;
    case 0x10:  // SPI_READ
      set_subcommand_reply();
      spi_read();
      break;
    case 0x03:  // SET_MODE
      set_subcommand_reply();
      set_mode();
      break;
    case 0x04:  // TRIGGER_BUTTONS
      set_subcommand_reply();
      set_trigger_buttons();
      break;
    case 0x40:  // TOGGLE_IMU
      set_subcommand_reply();
      toggle_imu();
      break;
    case 0x41:  // IMU_SENSITIVITY
      set_subcommand_reply();
      imu_sensitivity();
      break;
    case 0x48:  // ENABLE_VIBRATION
      set_subcommand_reply();
      enable_vibration();
      break;
    case 0x30:  // SET_PLAYER
      set_subcommand_reply();
      set_player_lights();
      break;
    case 0x22:  // SET_NFC_IR_STATE
      set_subcommand_reply();
      set_nfc_ir_state();
      break;
    case 0x21:  // SET_NFC_IR_CONFIG
      set_subcommand_reply();
      set_nfc_ir_config();
      break;
    default:
      set_full_input_report();
      break;
  }
  return _report;
}

void SwitchBluetooth::set_empty_report() {
  memset(_report, 0x00, sizeof(_report));
}

void SwitchBluetooth::set_empty_switch_request_report() {
  memset(_switchRequestReport, 0x00, sizeof(_switchRequestReport));
}

void SwitchBluetooth::set_subcommand_reply() {
  // Input Report ID
  _report[1] = 0x21;

  // TODO: Find out what the vibrator byte is doing.
  if (_vibration_enabled) {
    const uint8_t VIB_OPTS[4] = {0x0a, 0x0c, 0x0b, 0x09};
    _vibration_idx = (_vibration_idx + 1) % 4;
    _vibration_report = VIB_OPTS[_vibration_idx];
  }

  set_standard_input_report();
}

void SwitchBluetooth::set_timer() {
  // If the timer hasn't been set before
  if (_timestamp == 0) {
    _timestamp = to_ms_since_boot(get_absolute_time());
    _report[2] = 0x00;
    return;
  }

  // Get the time that has passed since the last timestamp
  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t delta_t = (now - _timestamp);

  // Get how many ticks have passed in hex with overflow at 255
  uint32_t elapsed_ticks = int(delta_t * 4);
  _timer = (_timer + elapsed_ticks) & 0xFF;

  _report[2] = _timer;
  _timestamp = now;
}

void SwitchBluetooth::set_full_input_report() {
  // Setting Report ID to full standard input report ID
  _report[1] = 0x30;

  set_standard_input_report();
  set_imu_data();
}

void SwitchBluetooth::set_standard_input_report() {
  set_timer();

  memcpy(_report + 3, (uint8_t *)&_switchReport, sizeof(SwitchReport));
  _report[13] = _vibration_report;
}

void SwitchBluetooth::set_bt() {
  _report[14] = 0x81;
  _report[15] = 0x01;
  _report[16] = 0x03;
}

void SwitchBluetooth::set_device_info() {
  // ACK Reply
  _report[14] = 0x82;
  // Subcommand Reply
  _report[15] = 0x02;
  // Firmware version
  _report[16] = 0x03;
  _report[17] = 0x48;
  // Controller ID
  _report[18] = 0x03;
  // Unknown Byte, always 2
  _report[19] = 0x02;
  // Controller Bluetooth Address
  memcpy(_report + 20, _addr, 6);
  // Unknown byte, always 1
  _report[26] = 0x01;
  // Controller colours location (read from SPI)
  _report[27] = 0x01;
}

void SwitchBluetooth::set_shipment() {
  _report[14] = 0x80;
  _report[15] = 0x08;
}

void SwitchBluetooth::toggle_imu() {
  _imu_enabled = _switchRequestReport[11] == 0x01;
  _report[14] = 0x80;
  _report[15] = 0x40;
}

void SwitchBluetooth::imu_sensitivity() {
  _report[14] = 0x80;
  _report[15] = 0x41;
}

void SwitchBluetooth::set_imu_data() {
  if (!_imu_enabled) {
    return;
  }

  uint8_t imu_data[49] = {0x75, 0xFD, 0xFD, 0xFF, 0x09, 0x10, 0x21, 0x00, 0xD5,
                          0xFF, 0xE0, 0xFF, 0x72, 0xFD, 0xF9, 0xFF, 0x0A, 0x10,
                          0x22, 0x00, 0xD5, 0xFF, 0xE0, 0xFF, 0x76, 0xFD, 0xFC,
                          0xFF, 0x09, 0x10, 0x23, 0x00, 0xD5, 0xFF, 0xE0, 0xFF};
  memcpy(_report + 14, imu_data, sizeof(imu_data));
}

void SwitchBluetooth::spi_read() {
  uint8_t addr_top = _switchRequestReport[12];
  uint8_t addr_bottom = _switchRequestReport[11];
  uint8_t read_length = _switchRequestReport[15];

  _report[14] = 0x90;
  _report[15] = 0x10;
  _report[16] = addr_bottom;
  _report[17] = addr_top;
  _report[20] = read_length;

  uint8_t params[18] = {0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54,
                        0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};

  if (addr_top == 0x60 && addr_bottom == 0x00) {
    memset(_report + 21, 0xff, 16);
  } else if (addr_top == 0x60 && addr_bottom == 0x50) {
    memset(_report + 21, 0x32, 3);
    memset(_report + 24, 0xff, 3);
    memset(_report + 27, 0xff, 7);
  } else if (addr_top == 0x60 && addr_bottom == 0x80) {
    _report[21] = 0x50; _report[22] = 0xFD; _report[23] = 0x00;
    _report[24] = 0x00; _report[25] = 0xC6; _report[26] = 0x0F;
    memcpy(_report + 27, params, sizeof(params));
  } else if (addr_top == 0x60 && addr_bottom == 0x98) {
    memcpy(_report + 21, params, sizeof(params));
  } else if (addr_top == 0x80 && addr_bottom == 0x10) {
    memset(_report + 21, 0xff, 3);
  } else if (addr_top == 0x60 && addr_bottom == 0x3D) {
    uint8_t l_calibration[9] = {0xD4, 0x75, 0x61, 0xE5, 0x87, 0x7C, 0xEC, 0x55, 0x61};
    uint8_t r_calibration[9] = {0x5D, 0xD8, 0x7F, 0x18, 0xE6, 0x61, 0x86, 0x65, 0x5D};
    memcpy(_report + 21, l_calibration, sizeof(l_calibration));
    memcpy(_report + 30, r_calibration, sizeof(r_calibration));
    _report[39] = 0xFF;
    memset(_report + 40, 0x32, 3);
    memset(_report + 43, 0xff, 3);
  } else if (addr_top == 0x60 && addr_bottom == 0x20) {
    uint8_t sa_calibration[24] = {0xcc, 0x00, 0x40, 0x00, 0x91, 0x01,
                                  0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
                                  0xe7, 0xff, 0x0e, 0x00, 0xdc, 0xff,
                                  0x3b, 0x34, 0x3b, 0x34, 0x3b, 0x34};
    memcpy(_report + 21, sa_calibration, sizeof(sa_calibration));
  } else {
    memset(_report + 21, 0xFF, read_length);
  }
}

void SwitchBluetooth::set_mode() {
  _report[14] = 0x80;
  _report[15] = 0x03;
}

void SwitchBluetooth::set_trigger_buttons() {
  _report[14] = 0x83;
  _report[15] = 0x04;
}

void SwitchBluetooth::enable_vibration() {
  _report[14] = 0x80;
  _report[15] = 0x48;
  _vibration_enabled = true;
  _vibration_idx = 0;
  const uint8_t VIB_OPTS[4] = {0x0a, 0x0c, 0x0b, 0x09};
  _vibration_report = VIB_OPTS[_vibration_idx];
}

void SwitchBluetooth::set_player_lights() {
  _report[14] = 0x80;
  _report[15] = 0x30;

  uint8_t bitfield = _switchRequestReport[11];
  if (bitfield == 0x01 || bitfield == 0x10) {
    _player_number = 1;
  } else if (bitfield == 0x03 || bitfield == 0x30) {
    _player_number = 2;
  } else if (bitfield == 0x07 || bitfield == 0x70) {
    _player_number = 3;
  } else if (bitfield == 0x0F || bitfield == 0xF0) {
    _player_number = 4;
  }
}

void SwitchBluetooth::set_nfc_ir_state() {
  _report[14] = 0x80;
  _report[15] = 0x22;
}

void SwitchBluetooth::set_nfc_ir_config() {
  _report[14] = 0xA0;
  _report[15] = 0x21;
  uint8_t params[8] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
  memcpy(_report + 16, params, sizeof(params));
  _report[49] = 0xC8;
}

void SwitchBluetooth::set_controller_rumble(bool rumble) {
  // No physical rumble to set in our case
}

void packet_handler(SwitchBluetooth *inst, uint8_t packet_type, uint8_t *packet) {
  if (packet_type != HCI_EVENT_PACKET || packet[0] != HCI_EVENT_HID_META) {
    return; // Fast exit for irrelevant packets
  }
  
  uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
  
  switch (subevent) {
    case HID_SUBEVENT_CONNECTION_OPENED:
      {
        uint8_t status = hid_subevent_connection_opened_get_status(packet);
        if (status) {
          FastLogger::log("Connection failed");
          inst->setHidCid(0);
        } else {
          FastLogger::log("Switch connected - ready for commands");
          inst->setHidCid(hid_subevent_connection_opened_get_hid_cid(packet));
          hid_device_request_can_send_now_event(inst->getHidCid());
        }
      }
      break;
      
    case HID_SUBEVENT_CONNECTION_CLOSED:
      FastLogger::log("Switch disconnected");
      inst->setHidCid(0);
      break;
      
    case HID_SUBEVENT_CAN_SEND_NOW:
      {
        try {
          // Process any remaining queued commands before generating report
          inst->process_command_queue();
          
          uint8_t *report = inst->generate_report();
          hid_device_send_interrupt_message(inst->getHidCid(), report, 50);
          inst->set_empty_switch_request_report();
          
          // Mark report as sent for timing control (applies to all reports)
          inst->mark_report_sent();
          
          hid_device_request_can_send_now_event(inst->getHidCid());
        } catch (int e) {
          hid_device_request_can_send_now_event(inst->getHidCid());
        }
      }
      break;
      
    default:
      // Ignore other subevent types
      break;
  }
}

void hid_report_data_callback(SwitchBluetooth *inst, uint16_t report_id, uint8_t *report, int report_size) {
  // Skip rumble processing for performance - not needed for macro execution
  inst->setSwitchRequestReport(report, report_size);
}