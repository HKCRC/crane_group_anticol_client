#include "eth_manage_task.h"

#include "lwip.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
/* UDP receive and parse for load weight */
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "board_manage.h"
#include "modbus_register_interface.h"
#include "modbus_register_database.h"

typedef void (*tcpip_callback_fn)(void *ctx);
extern err_t tcpip_callback(tcpip_callback_fn function, void *ctx);

#define LOG_D(fmt, ...) // printf("[D][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define LOG_I(fmt, ...) printf("[I][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define LOG_E(fmt, ...) printf("[E][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)

#define READ_LOAD_TASK_UDP_PORT 1212U
#define READ_LOAD_TASK_PROTOCOL_ID_LE 0x33532D00UL
#define READ_LOAD_TASK_MAIN_INDEX 0x0101U
#define READ_LOAD_TASK_FRAME_MAGIC_0 0x73U
#define READ_LOAD_TASK_FRAME_MAGIC_1 0x17U
#define READ_LOAD_TASK_LIFT_MOMENT_OFFSET 32U
#define READ_LOAD_TASK_LIFT_MOMENT_PCT_OFFSET 36U
#define READ_LOAD_TASK_WEIGHT_OFFSET 40U
#define READ_LOAD_TASK_WEIGHT_PCT_OFFSET 44U
#define READ_LOAD_TASK_HOOK_LOWERING_HEIGHT_OFFSET 48U
#define READ_LOAD_TASK_HEADER_LEN 20U
#define READ_LOAD_TASK_PROTOCOL_ID_OFFSET 0U
#define READ_LOAD_TASK_MAIN_INDEX_OFFSET 8U
#define READ_LOAD_TASK_FRAME_LEN_OFFSET 14U
#define READ_LOAD_TASK_FRAME_VERSION_OFFSET 20U
#define READ_LOAD_TASK_COPY_BUFFER_SIZE 512U

static volatile float s_latest_load_weight_t = 0.0f;
static volatile bool s_load_weight_valid = false;
static volatile float s_latest_lifting_moment = 0.0f;
static volatile float s_latest_lifting_moment_pct = 0.0f;
static volatile float s_latest_load_weight_pct = 0.0f;
static volatile float s_latest_hook_lowering_height = 0.0f;
static struct udp_pcb *s_udp_pcb = NULL;
static uint8_t s_udp_payload[READ_LOAD_TASK_COPY_BUFFER_SIZE];

static float read_f32_le(const uint8_t *data)
{
  float v = 0.0f;
  memcpy(&v, data, sizeof(v));
  return v;
}

static uint16_t read_u16_le(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static bool parse_load_metrics(const uint8_t *payload,
                               uint16_t payload_len,
                               float *lifting_moment,
                               float *lifting_moment_pct,
                               float *load_weight,
                               float *load_weight_pct,
                               float *hook_lowering_height)
{
  uint16_t required_len = READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_HOOK_LOWERING_HEIGHT_OFFSET + (uint16_t)sizeof(float);

  if ((payload == NULL) || (lifting_moment == NULL) || (lifting_moment_pct == NULL) ||
      (load_weight == NULL) || (load_weight_pct == NULL) || (hook_lowering_height == NULL) ||
      (payload_len < required_len)) {
    return false;
  }

  if (read_u32_le(&payload[READ_LOAD_TASK_PROTOCOL_ID_OFFSET]) != READ_LOAD_TASK_PROTOCOL_ID_LE) {
    return false;
  }

  if (read_u16_le(&payload[READ_LOAD_TASK_MAIN_INDEX_OFFSET]) != READ_LOAD_TASK_MAIN_INDEX) {
    return false;
  }

  if (read_u16_le(&payload[READ_LOAD_TASK_FRAME_LEN_OFFSET]) != payload_len) {
    return false;
  }

  if ((payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET] != READ_LOAD_TASK_FRAME_MAGIC_0) ||
      (payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + 1U] != READ_LOAD_TASK_FRAME_MAGIC_1)) {
    return false;
  }

  *lifting_moment = read_f32_le(&payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_LIFT_MOMENT_OFFSET]);
  *lifting_moment_pct = read_f32_le(&payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_LIFT_MOMENT_PCT_OFFSET]);
  *load_weight = read_f32_le(&payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_WEIGHT_OFFSET]);
  *load_weight_pct = read_f32_le(&payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_WEIGHT_PCT_OFFSET]);
  *hook_lowering_height = read_f32_le(&payload[READ_LOAD_TASK_FRAME_VERSION_OFFSET + READ_LOAD_TASK_HOOK_LOWERING_HEIGHT_OFFSET]);
  return true;

}

static void eth_udp_recv(void *arg,
             struct udp_pcb *pcb,
             struct pbuf *p,
             const ip_addr_t *addr,
             u16_t port)
{
  (void)arg; (void)pcb; (void)addr; (void)port;

  if (p == NULL) return;

  uint16_t payload_len = p->tot_len;
  if (payload_len > sizeof(s_udp_payload)) payload_len = sizeof(s_udp_payload);

  if (pbuf_copy_partial(p, s_udp_payload, payload_len, 0) == payload_len) {
    float lifting_moment = 0.0f;
    float lifting_moment_pct = 0.0f;
    float load_weight = 0.0f;
    float load_weight_pct = 0.0f;
    float hook_lowering_height = 0.0f;
    if (parse_load_metrics(s_udp_payload, payload_len, &lifting_moment, &lifting_moment_pct,
                           &load_weight, &load_weight_pct, &hook_lowering_height)) {
      s_latest_lifting_moment = lifting_moment;
      s_latest_lifting_moment_pct = lifting_moment_pct;
      s_latest_load_weight_t = load_weight;
      s_latest_load_weight_pct = load_weight_pct;
      s_latest_hook_lowering_height = hook_lowering_height;
      s_load_weight_valid = true;
      
      /* Write load metrics to Modbus registers */
      if (mb_reg_write_float(REG_LIFTING_MOMENT, lifting_moment) != ERR_OK) {
        LOG_E("Failed to write lifting moment to Modbus register\n");
      }
      if (mb_reg_write_float(REG_LIFTING_MOMENT_PCT, lifting_moment_pct) != ERR_OK) {
        LOG_E("Failed to write lifting moment percentage to Modbus register\n");
      }
      if (mb_reg_write_float(REG_LOAD_WEIGHT, load_weight) != ERR_OK) {
        LOG_E("Failed to write load weight to Modbus register\n");
      }
      if (mb_reg_write_float(REG_LOAD_WEIGHT_PCT, load_weight_pct) != ERR_OK) {
        LOG_E("Failed to write load weight percentage to Modbus register\n");
      }
      if (mb_reg_write_float(REG_HOOK_LOWERING_HEIGHT, hook_lowering_height) != ERR_OK) {
        LOG_E("Failed to write hook lowering height to Modbus register\n");
      }
      
      /* Set data valid bit for load weight */
      mb_reg_write_bit(REG_DATA_VAILD, REG_DATA_VAILD_BIT_LOAD_WEIGHT, true);
      
      LOG_D("Moment:%.3f t*m, MomentPct:%.3f%%, Weight:%.3f t, WeightPct:%.3f%%, HookDown:%.3f m\n",
        lifting_moment, lifting_moment_pct, load_weight, load_weight_pct, hook_lowering_height);
    }
  } else {
    LOG_E("Failed to copy UDP payload\n");
  }

  pbuf_free(p);
}

static void eth_udp_listener_init(void *ctx)
{
  (void)ctx;

  if (s_udp_pcb != NULL) {
    return;
  }

  s_udp_pcb = udp_new();
  if (s_udp_pcb == NULL) {
    LOG_E("Failed to create UDP pcb in eth_manage_task\n");
    return;
  }

  if (udp_bind(s_udp_pcb, IP_ADDR_ANY, READ_LOAD_TASK_UDP_PORT) != ERR_OK) {
    LOG_E("Failed to bind UDP port %u in eth_manage_task\n", READ_LOAD_TASK_UDP_PORT);
    udp_remove(s_udp_pcb);
    s_udp_pcb = NULL;
    return;
  }

  udp_recv(s_udp_pcb, eth_udp_recv, NULL);
  LOG_I("eth_manage_task listening UDP port %u for load frames\n", READ_LOAD_TASK_UDP_PORT);
}


#define ETH_STATIC_IP_ADDR "192.168.1.33"
#define ETH_STATIC_NETMASK "255.255.255.0"
#define ETH_STATIC_GATEWAY "192.168.1.1"

int eth_link_up_timeout_ms = 60 * 1000;
#include "ota_service_task.h"
extern ota_service_t g_ota;
void eth_manage_task(void *argument) {

    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_RESET);
    osDelay(100);
    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_SET);
    osDelay(100);

    ip4_addr_t static_ip, static_netmask, static_gateway;
    ip4addr_aton(ETH_STATIC_IP_ADDR, &static_ip);
    ip4addr_aton(ETH_STATIC_NETMASK, &static_netmask);
    ip4addr_aton(ETH_STATIC_GATEWAY, &static_gateway);
    lwip_set_net_param(false, static_ip.addr, static_netmask.addr, static_gateway.addr);
    MX_LWIP_Init();

    extern struct netif gnetif;
    while (!netif_is_link_up(&gnetif) || !netif_is_up(&gnetif))
    {
      osDelay(100);
      offline_manage_update_event(OFFLINE_ETH_MANAGE);
    }

    /* Setup UDP listener for load weight frames in tcpip thread context */
    if (tcpip_callback(eth_udp_listener_init, NULL) != ERR_OK) {
      LOG_E("Failed to schedule UDP listener init in tcpip thread\n");
    }
    
    while (1) 
    {
      if (g_ota.state != OTA_SVC_IDLE)
      {
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
      }

      osDelay(100);
      offline_manage_update_event(OFFLINE_ETH_MANAGE);
    }
}
