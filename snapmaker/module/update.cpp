#include "update.h"
#include "../../marlin/src/core/serial.h"
#include "flash_stm32.h"
#include HAL_PATH(src/HAL, HAL_watchdog_STM32F1.h)

UpdateServer update_server;


#define APP_FLASH_PAGE_SIZE  (2*1024)
#define DATA_FLASH_PAGE_SIZE  (4*1024)  // after 512K

#define FLASH_BASE (0x08000000)
#define DATA_FLASH_START_ADDR (FLASH_BASE + (512 * 1024))
#define FLASH_UPDATE_INFO_ADDR (FLASH_BASE + 1024*1012) 

uint32_t update_calc_checksum(uint8_t *buffer, uint32_t length) {
  uint32_t volatile checksum = 0;

  if (!length || !buffer)
    return 0;

  for (uint32_t j = 0; j < (length - 1); j = j + 2)
    checksum += (uint32_t)(buffer[j] << 8 | buffer[j + 1]);

  if (length % 2)
    checksum += buffer[length - 1];

  checksum = ~checksum;

  return checksum;
}

void erase_flash_page(uint32_t addr, uint16_t page_count) {
  FLASH_Unlock();
  for (int i = 0; i < page_count; i++) {
    FLASH_ErasePage(addr);
    if (addr < DATA_FLASH_START_ADDR)
      addr += APP_FLASH_PAGE_SIZE;
    else
      addr += DATA_FLASH_PAGE_SIZE;
  }
  FLASH_Lock();
}

void write_to_flash(uint32_t addr, uint8_t *data, uint32_t len) {
  uint16_t tmp;
  FLASH_Unlock();
  for (uint32_t i = 0; (i + 2) <= len; i = i + 2) {
    tmp = ((data[i + 1]<<8) | data[i]);
    FLASH_ProgramHalfWord(addr, tmp);
    addr = addr + 2;
  }
  if (len % 2) {
    tmp = ((data[len - 1]) | 0xFF00);
    FLASH_ProgramHalfWord(addr, tmp);
  }
  FLASH_Lock();
}

uint32_t UpdateServer::update_packet_head_checksum(update_packet_info_t *head) {
  uint32_t checksum = update_calc_checksum((uint8_t *)head, sizeof(update_packet_info_t) - sizeof(head->pack_head_checknum));
  return checksum;
}

ErrCode  UpdateServer::update_info_check(update_packet_info_t *head) {
  uint32_t checksum = update_packet_head_checksum(head);
  if (checksum != head->pack_head_checknum) {
    return E_PARAM;
  }

  if (head->app_flash_start_addr % APP_FLASH_PAGE_SIZE) {
    return E_PARAM;
  }

  if (head->app_flash_start_addr < FLASH_BASE + BOOT_CODE_SIZE) {
    return E_PARAM;
  }

  return E_SUCCESS;
}

void UpdateServer::set_update_status(uint16_t status) {
  update_packet_info_t *flash_info = (update_packet_info_t *)FLASH_UPDATE_INFO_ADDR;
  update_packet_info_t info;
  memcpy((uint8_t *)&info, (uint8_t *)flash_info, sizeof(update_packet_info_t));
  info.status_flag = status;
  uint32_t checksum = update_packet_head_checksum(&info);
  info.pack_head_checknum = checksum;

  erase_flash_page(FLASH_UPDATE_INFO_ADDR, 1);
  write_to_flash(FLASH_UPDATE_INFO_ADDR, (uint8_t*)&info, sizeof(update_packet_info_t));
}

void UpdateServer::save_update_info(update_packet_info_t * info, uint8_t usart_num, uint8_t receiver_id) {
  info->status_flag = UPDATE_STATUS_START;
  info->usart_num = usart_num;
  info->receiver_id = receiver_id;
  uint32_t checksum = update_packet_head_checksum(info);
  info->pack_head_checknum = checksum;
  erase_flash_page(FLASH_UPDATE_INFO_ADDR, 1);
  write_to_flash(FLASH_UPDATE_INFO_ADDR, (uint8_t*)info, sizeof(update_packet_info_t));
}

ErrCode UpdateServer::is_allow_update(update_packet_info_t *head) {
  ErrCode ret = update_info_check(head);
  return ret;
}

void UpdateServer::just_to_boot() {
  WatchDogInit();
}

void UpdateServer::init() {
  update_packet_info_t *update_info =  (update_packet_info_t *)FLASH_UPDATE_INFO_ADDR;
  if (update_info->status_flag != UPDATE_STATUS_APP_NORMAL) {
    set_update_status(UPDATE_STATUS_APP_NORMAL);
  }
}