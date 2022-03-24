#include "esp_err.h"
#include "tcpip_adapter.h"

esp_err_t connect(void);

esp_err_t disconnect(void);

esp_err_t set_connection_info(const char *ssid, const char *passwd);
