#include "event_printer.h"
#include "../module/print_control.h"
#include "../module/power_loss.h"
#include "../module/filament_sensor.h"
#include "../module/system.h"
#include "../module/fdm.h"

#define GCODE_MAX_PACK_SIZE 450
#define GCODE_REQ_TIMEOUT_MS 2000

#pragma pack(1)

typedef struct {
  uint16_t md5_length;
  uint8_t md5_str[GCODE_MD5_LENGTH];
  uint16_t name_size;
  uint8_t name[GCODE_FILE_NAME_SIZE];
} gcode_file_info_t;

typedef struct {
  uint32_t line_number;
  uint16_t buf_max_size;
} batch_gcode_req_info_t;

typedef struct {
  uint8_t flag;
  uint32_t start_line;
  uint32_t end_line;
  uint16_t  data_len;
  uint8_t data[];
} batch_gcode_t;

typedef struct {
  uint8_t key;
  uint8_t e_index;
  float_to_int_t percentage;
} feedrate_percentage_t;


#pragma pack()

typedef enum {
  GCODE_PACK_REQ_IDLE,
  GCODE_PACK_REQ_WAIT_RECV,
  GCODE_PACK_REQ_WAIT_CACHE,
  GCODE_PACK_REQ_DONE,
} gcode_req_status_e;

typedef enum {
  STATUS_PRINT_DONE,
  STATUS_PAUSE_BE_GCODE,
  STATUS_PAUSE_BE_GCODE_FILAMENT,
  STATUS_PAUSE_BE_FILAMENT,
  STATUS_STALL_GUARD,
  STATUS_TEMPERATURE_ERR,
  STATUS_GCODE_LINES_ERR,
} report_status_e;


// 这里的变量应该统一结构体管理
event_source_e print_source = EVENT_SOURCE_HMI;
uint8_t source_recever_id = 0;
uint16_t source_sequence = 0;
gcode_req_status_e gcode_req_status = GCODE_PACK_REQ_IDLE;
uint32_t gcode_req_timeout = 0;

static void req_gcode_pack();

static void save_event_suorce_info(event_param_t& event) {
  print_source = event.source;
  source_recever_id = event.info.recever_id;
  source_sequence = event.info.sequence;
}

static uint16_t load_gcode_file_info(uint8_t *buf) {
  uint16_t ret_len = 0;
  uint8_t temp_len = 0;
  uint8_t * addr = power_loss.get_file_md5(temp_len);

  if (addr) {
    memcpy(buf+2, addr, temp_len);
    *((uint16_t *)buf) = temp_len;
    ret_len += temp_len + 2;
  } else {
    SERIAL_ECHOLNPAIR("load gcode file MD5 failed");
  }

  buf += (temp_len + 2);
  addr = power_loss.get_file_name(temp_len);
  if (addr) {
    memcpy(buf+2, addr, temp_len);
    *((uint16_t *)buf) = temp_len;
    ret_len += temp_len + 2;
  } else {
    SERIAL_ECHOLNPAIR("load gcode file name failed");
  }
  return ret_len;
}

static ErrCode request_file_info(event_param_t& event) {
  uint16_t ret_len = load_gcode_file_info(event.data+1);
  SERIAL_ECHOLN("SC req file info");
  if (ret_len) {
    event.data[0] = E_SUCCESS;
    event.length = ret_len + 1;
  } else {
    event.data[0] = PRINT_RESULT_NO_FILE_INFO_E;
    event.length = 5;
  }
  event.length = sizeof(gcode_file_info_t) + 1;
  return send_event(event);
}

static ErrCode gcode_pack_deal(event_param_t& event) {
  batch_gcode_t *gcode = (batch_gcode_t *)event.data;
  print_control.push_gcode(gcode->start_line, gcode->end_line, gcode->data, gcode->data_len);
  if (gcode->flag == PRINT_RESULT_GCODE_RECV_DONE_E) {
    gcode_req_status = GCODE_PACK_REQ_DONE;
    SERIAL_ECHOLN("SC gcoce pack recv done");
  } else {
    req_gcode_pack();
  }
  return E_SUCCESS;
}

static ErrCode request_start_work(event_param_t& event) {
  SERIAL_ECHOLNPAIR("SC req start work");
  ErrCode result= print_control.start();
  SERIAL_ECHOLNPAIR("start work result:", result);
  if (result == E_SUCCESS) {
    // set md5 and file name
    uint16_t data_len = *((uint16_t *)event.data);
    uint8_t *data = event.data + 2;
    power_loss.set_file_md5(data, data_len);
    data = event.data + data_len + 4;
    data_len = *((uint16_t *)&event.data[data_len + 2]);
    power_loss.set_file_name(data, data_len);

    save_event_suorce_info(event);
  }
  event.data[0] = result;
  event.length = 1;
  send_event(event);
  if (result == E_SUCCESS) {
    req_gcode_pack();
  }
  return result;
}

static ErrCode request_pause_work(event_param_t& event) {
  if (system_service.get_status() != SYSTEM_STATUE_PRINTING) {
    event.data[0] = PRINT_RESULT_PAUSE_ERR_E;
    event.length = 1;
    SERIAL_ECHOLNPAIR("SC req pause work failed");
    return send_event(event);
  }
  system_service.set_status(SYSTEM_STATUE_PAUSING, SYSTEM_STATUE_SCOURCE_SACP);
  gcode_req_status = GCODE_PACK_REQ_IDLE;
  save_event_suorce_info(event);
  SERIAL_ECHOLNPAIR("SC req pause working...");
  return E_SUCCESS;
}

static ErrCode request_resume_work(event_param_t& event) {
   if (system_service.get_status() != SYSTEM_STATUE_PAUSED) {
    event.data[0] = PRINT_RESULT_RESUME_ERR_E;
    event.length = 1;
    SERIAL_ECHOLNPAIR("SC req pause work failed");
    return send_event(event);
  }
  save_event_suorce_info(event);
  system_service.set_status(SYSTEM_STATUE_RESUMING, SYSTEM_STATUE_SCOURCE_SACP);
  SERIAL_ECHOLNPAIR("SC req resume work");
  return E_SUCCESS;
}

static ErrCode request_stop_work(event_param_t& event) {
  SERIAL_ECHOLNPAIR("SC req stop work");
  system_service.set_status(SYSTEM_STATUE_STOPPING, SYSTEM_STATUE_SCOURCE_SACP);
  gcode_req_status = GCODE_PACK_REQ_IDLE;
  save_event_suorce_info(event);
  return E_SUCCESS;
}

static ErrCode request_stop_single_extrude_work(event_param_t& event) {
  uint8_t e = MODULE_INDEX(event.data[0]);
  uint8_t en = event.data[1];
  SERIAL_ECHOLNPAIR("SC req extrude:", e, " enable:", en);

  if (system_service.get_status() == SYSTEM_STATUE_IDLE && !fdm_head.is_duplicating()) {
    event.data[0] = PRINT_RESULT_START_ERR_E;
    event.length = 1;
    SERIAL_ECHOLNPAIR("SC req single stop work failed");
    return send_event(event);
  }

  // Printing can only be stopped.
  // This function will be enabled again when printing ends
  save_event_suorce_info(event);
  if (system_service.get_status() != SYSTEM_STATUE_PRINTING) {
    power_loss.stash_data.extruder_dual_enable[e] = en;
    event.data[0] = E_SUCCESS;
    event.length = 1;
    return send_event(event);
  }

  fdm_head.set_duplication_enabled(e, en);
  system_service.set_status(SYSTEM_STATUE_PAUSING, SYSTEM_STATUE_SCOURCE_STOP_EXTRUDE);
  return E_SUCCESS;
}

static ErrCode request_power_loss_status(event_param_t& event) {
  event.data[0] = power_loss.is_power_loss_data();
  SERIAL_ECHOLNPAIR("SC req power loos status:", event.data[0]);
  if (event.data[0] == E_SUCCESS) {
    uint16_t ret_len = load_gcode_file_info(event.data+1);
    event.length = ret_len + 1;
  } else {
    event.length = 5;
  }
  return send_event(event);
}

static ErrCode request_power_loss_resume(event_param_t& event) {
  SERIAL_ECHOLNPAIR("SC req power loss resume");
  event.data[0] = power_loss.power_loss_resume();
  event.data[0] = E_SUCCESS;
  event.length = 1;
  send_event(event);
  if (event.data[0] == E_SUCCESS) {
    SERIAL_ECHOLNPAIR("power loss resume success");
    save_event_suorce_info(event);
    req_gcode_pack();
  } else {
    SERIAL_ECHOLNPAIR("power loss resume failed");
  }
  return E_SUCCESS;
}

static ErrCode request_clear_power_loss(event_param_t& event) {
  SERIAL_ECHOLNPAIR("SC req power loos clear");
  power_loss.clear();
  event.data[0] = E_SUCCESS;
  event.length = 1;
  return send_event(event);
}

static ErrCode set_printer_mode(event_param_t& event) {
  SERIAL_ECHOLNPAIR("SC set print mode:", event.data[0]);
  event.data[0] = print_control.set_mode((print_mode_e)event.data[0]);
  event.length = 1;
  return send_event(event);
}

static ErrCode request_auto_pack_status(event_param_t& event) {
  bool status = print_control.is_backup_mode();
  SERIAL_ECHOLNPAIR("SC req auto pack mode:", status);
  event.data[0] = E_SUCCESS;
  event.data[1] = status;
  event.length = 2;
  return send_event(event);
}

static ErrCode set_auto_pack_mode(event_param_t& event) {
  print_mode_e mode = PRINT_BACKUP_MODE;
  if (event.data[0])
    mode = PRINT_AUTO_PARK_MODE;
  SERIAL_ECHOLNPAIR("SC req auto pack mode:", mode);
  event.data[0] = print_control.set_mode(mode);
  event.length = 1;
  return send_event(event);
}

static ErrCode request_cur_line(event_param_t& event) {
  event.data[0] = E_SUCCESS;
  uint32_t *cur_line = (uint32_t *)(event.data+1);
  *cur_line = print_control.get_cur_line();
  event.length = 5;
  return send_event(event);
}

static ErrCode request_temperature_lock(event_param_t& event) {
  if (system_service.get_status() == SYSTEM_STATUE_IDLE) {
    event.data[0] = 1;
  }
  event.length = 1;
  return send_event(event);
}

static ErrCode set_work_feedrate_percentage(event_param_t& event) {
  feedrate_percentage_t *info = (feedrate_percentage_t *)event.data;
  float percent = INT_TO_FLOAT(info->percentage);
  SERIAL_ECHOLNPAIR_F("SC set feedrate percentage:", percent);
  print_control.set_feedrate_percentage(percent);
  event.data[0] = E_SUCCESS;
  event.length = 1;
  return send_event(event);
}

event_cb_info_t printer_cb_info[PRINTER_ID_CB_COUNT] = {
  {PRINTER_ID_REQ_FILE_INFO       , EVENT_CB_DIRECT_RUN, request_file_info},
  {PRINTER_ID_REQ_GCODE           , EVENT_CB_TASK_RUN,   gcode_pack_deal},
  {PRINTER_ID_START_WORK          , EVENT_CB_TASK_RUN,   request_start_work},
  {PRINTER_ID_PAUSE_WORK          , EVENT_CB_DIRECT_RUN,   request_pause_work},
  {PRINTER_ID_RESUME_WORK         , EVENT_CB_DIRECT_RUN,   request_resume_work},
  {PRINTER_ID_STOP_WORK           , EVENT_CB_TASK_RUN,   request_stop_work},
  {PRINTER_ID_REQ_PL_STATUS       , EVENT_CB_DIRECT_RUN, request_power_loss_status},
  {PRINTER_ID_PL_RESUME           , EVENT_CB_TASK_RUN,   request_power_loss_resume},
  {PRINTER_ID_CLEAN_PL_DATA       , EVENT_CB_TASK_RUN,   request_clear_power_loss},
  {PRINTER_ID_SET_MODE            , EVENT_CB_TASK_RUN,   set_printer_mode},
  {PRINTER_ID_REQ_AUTO_PARK_STATUS, EVENT_CB_DIRECT_RUN, request_auto_pack_status},
  {PRINTER_ID_SET_AUTO_PARK_STATUS, EVENT_CB_TASK_RUN,   set_auto_pack_mode},
  {PRINTER_ID_STOP_SINGLE_EXTRUDE , EVENT_CB_DIRECT_RUN,   request_stop_single_extrude_work},
  {PRINTER_ID_SET_WORK_PERCENTAGE , EVENT_CB_DIRECT_RUN,   set_work_feedrate_percentage},
  {PRINTER_ID_REQ_LINE            , EVENT_CB_DIRECT_RUN, request_cur_line},
  {PRINTER_ID_TEMPERATURE_LOCK    , EVENT_CB_DIRECT_RUN, request_temperature_lock},
};

static void req_gcode_pack() {
  batch_gcode_req_info_t info;
  uint16_t free_buf = print_control.get_buf_free();
  // SERIAL_ECHOLNPAIR("gcode buf free:", free_buf);
  if (free_buf >= GCODE_MAX_PACK_SIZE) {
    info.line_number = print_control.next_req_line();
    info.buf_max_size = GCODE_MAX_PACK_SIZE;
    send_event(print_source, source_recever_id, SACP_ATTR_REQ,
        COMMAND_SET_PRINTER, PRINTER_ID_REQ_GCODE, (uint8_t *)&info, sizeof(info));
    gcode_req_status = GCODE_PACK_REQ_WAIT_RECV;
    gcode_req_timeout = millis() + GCODE_REQ_TIMEOUT_MS;
    SERIAL_ECHOLNPAIR("gcode requst start line:", info.line_number, ",size:", info.buf_max_size);
  } else {
    gcode_req_status = GCODE_PACK_REQ_WAIT_CACHE;
  }
}

static void gcode_req_timeout_deal() {
  if (gcode_req_timeout < millis()) {
    SERIAL_ECHOLNPAIR("requst gcode pack timeout!");
    req_gcode_pack();
  }
}

static void report_status_info(ErrCode status) {
  send_event(print_source, source_recever_id, SACP_ATTR_REQ,
      COMMAND_SET_PRINTER, PRINTER_ID_REPORT_STATUS, &status, 1);
}

void wait_print_end(void) {
  if (print_control.buffer_is_empty()) {
    SERIAL_ECHOLNPAIR("print done and will stop");
    gcode_req_status = GCODE_PACK_REQ_IDLE;
    system_service.set_status(SYSTEM_STATUE_STOPPING, SYSTEM_STATUE_SCOURCE_DONE);
  }
}

void pausing_status_deal() {
  ErrCode result = print_control.pause();
  switch (system_service.get_source()) {
    case SYSTEM_STATUE_SCOURCE_FILAMENT:
      report_status_info(STATUS_PAUSE_BE_FILAMENT);
      SERIAL_ECHOLNPAIR("lilament puase done");
      break;
    case SYSTEM_STATUE_SCOURCE_GCODE:
      report_status_info(STATUS_PAUSE_BE_GCODE);
      SERIAL_ECHOLNPAIR("gcode puase done");
      break;
    case SYSTEM_STATUE_SCOURCE_TOOL_CHANGE:
      SERIAL_ECHOLNPAIR("change tool head continue");
      power_loss.change_head();
      print_control.resume();
      req_gcode_pack();
      break;
    case SYSTEM_STATUE_SCOURCE_STOP_EXTRUDE:
      SERIAL_ECHOLNPAIR("stop single extrude done and continue");
      result = print_control.resume();
      send_event(print_source, source_recever_id, SACP_ATTR_ACK,
                  COMMAND_SET_PRINTER, PRINTER_ID_STOP_SINGLE_EXTRUDE, &result, 1, source_sequence);
      req_gcode_pack();
      break;
    
    default:
      send_event(print_source, source_recever_id, SACP_ATTR_ACK,
                  COMMAND_SET_PRINTER, PRINTER_ID_PAUSE_WORK, &result, 1, source_sequence);
      SERIAL_ECHOLNPAIR("system puase done:", result);
      break;
  }
}

void printer_event_init(void) {
  filament_sensor.init();
  power_loss.init();
}

void printing_status_deal() {
  switch (gcode_req_status) {
    case GCODE_PACK_REQ_WAIT_CACHE:
      req_gcode_pack();
      break;
    case GCODE_PACK_REQ_WAIT_RECV:
      gcode_req_timeout_deal();
      break;
    case GCODE_PACK_REQ_DONE:
      wait_print_end();
      break;
    default:
      break;
  }
}

void paused_status_deal() {
  // 如果5分钟没有操作就关闭电机进入休眠状态
}

void resuming_status_deal() {
  uint8_t data[7];
  batch_gcode_req_info_t *info = (batch_gcode_req_info_t *)(data + 1);
  ErrCode result = print_control.resume();
  SERIAL_ECHOLNPAIR("resume work success, ret:", result);
  data[0] = result;
  info->buf_max_size = GCODE_MAX_PACK_SIZE;
  info->line_number = print_control.next_req_line();
  send_event(print_source, source_recever_id, SACP_ATTR_ACK,
    COMMAND_SET_PRINTER, PRINTER_ID_RESUME_WORK, data, 7, source_sequence);
  req_gcode_pack();
}

void stopping_status_deal() {
  ErrCode result = E_SUCCESS;
  SERIAL_ECHOLNPAIR("stop working...");
  result = print_control.stop();

  HOTEND_LOOP() {
    fdm_head.set_duplication_enabled(e, true);
    print_control.temperature_lock(e, false);
  }
  print_control.set_feedrate_percentage(100);
  if (system_service.get_source() == SYSTEM_STATUE_SCOURCE_SACP) {
    send_event(print_source, source_recever_id, SACP_ATTR_ACK,
      COMMAND_SET_PRINTER, PRINTER_ID_STOP_WORK, &result, 1, source_sequence);
  } else {
    report_status_info(STATUS_PRINT_DONE);
  }
  SERIAL_ECHOLNPAIR("stop success");
}

void printer_event_loop(void) {
  switch (system_service.get_status()) {
    case SYSTEM_STATUE_PRINTING:
      printing_status_deal();
      break;
    case SYSTEM_STATUE_PAUSED:
      paused_status_deal();
      break;
    case SYSTEM_STATUE_PAUSING:
      pausing_status_deal();
      break;
    case SYSTEM_STATUE_RESUMING:
      resuming_status_deal();
      break;
    case SYSTEM_STATUE_STOPPING:
      stopping_status_deal();
      break;
    default:
      break; 
  }
}