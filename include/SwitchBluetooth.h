#ifndef SwitchBluetooth_h
#define SwitchBluetooth_h

#include "SwitchConsts.h"
#include "btstack.h"

class SwitchBluetooth {
 public:
  void init();
  void setHidCid(uint16_t hid_cid) { _hid_cid = hid_cid; };
  uint16_t getHidCid() { return _hid_cid; };
  uint8_t *generate_report();
  void setSwitchRequestReport(uint8_t *report, int report_size);
  void set_empty_switch_request_report();
  void set_controller_rumble(bool rumble);
  
  // Button control methods
  void set_button(const char* button, bool pressed);
  void set_stick(const char* stick, float h, float v);
  
  // Direct state modification (used internally by queue processor)
  void set_button_direct(const char* button, bool pressed);
  void set_stick_direct(const char* stick, float h, float v);
  
  // Bluetooth timing control
  bool can_send_hid_report();
  void mark_report_sent();
  bool has_pending_update() { return _pending_report_update; }
  bool has_config_request() { return _switchRequestReport[10] != 0x00; }
  bool is_paired() { return _device_info_queried; }
  void wait_for_hid_transmission();
  
  // Command queue and frame consolidation
  void start_consolidation();
  void end_consolidation();
  void queue_button_command(const char* button, bool pressed);
  void queue_stick_command(const char* stick, float h, float v);
  void process_command_queue();
  bool has_queued_commands();

 private:
  uint16_t _hid_cid = 0;
  SwitchReport _switchReport = {
      .batteryConnection = 0x91, .buttons = {0x0}, .l = {0x0}, .r = {0x0}};
  uint8_t _report[100] = {0x0};
  uint8_t _switchRequestReport[100] = {0x0};
  uint8_t _addr[6] = {0x0};
  bool _vibration_enabled = false;
  uint8_t _vibration_report = 0x00;
  uint8_t _vibration_idx = 0x00;
  bool _imu_enabled = false;
  uint8_t _player_number = 0x00;
  bool _device_info_queried = false;
  uint32_t _timer = 0;
  uint32_t _timestamp = 0;
  
  // Bluetooth timing control for 125Hz HID rate (8ms intervals)
  uint32_t _last_hid_report_time = 0;
  bool _pending_report_update = false;
  
  // Command queue system for reliable frame consolidation
  struct QueuedCommand {
    enum Type { BUTTON_PRESS, BUTTON_RELEASE, STICK_SET } type;
    char button_name[16];
    bool pressed;
    float stick_h, stick_v;
  };
  
  static const int MAX_QUEUE_SIZE = 32;
  QueuedCommand _command_queue[MAX_QUEUE_SIZE];
  volatile int _queue_head = 0;
  volatile int _queue_tail = 0;
  volatile bool _queue_full = false;
  
  // Frame consolidation system
  bool _consolidation_active = false;
  uint32_t _consolidation_start_time = 0;
  static const uint32_t CONSOLIDATION_WINDOW_MS = 3; // 3ms window for command consolidation
  
  // Helper methods (from SwitchCommon)
  void set_empty_report();
  void set_subcommand_reply();
  void set_unknown_subcommand(uint8_t subcommand_id);
  void set_timer();
  void set_full_input_report();
  void set_standard_input_report();
  void set_bt();
  void set_device_info();
  void set_shipment();
  void toggle_imu();
  void imu_sensitivity();
  void set_imu_data();
  void spi_read();
  void set_mode();
  void set_trigger_buttons();
  void enable_vibration();
  void set_player_lights();
  void set_nfc_ir_state();
  void set_nfc_ir_config();
};

void packet_handler(SwitchBluetooth *inst, uint8_t packet_type, uint8_t *packet);
void hid_report_data_callback(SwitchBluetooth *inst, uint16_t report_id, uint8_t *report, int report_size);

#endif