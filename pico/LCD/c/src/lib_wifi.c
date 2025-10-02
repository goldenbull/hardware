#include "lib_wifi.h"

void init_wifi()
{
    int ret = cyw43_arch_init();
    if (ret)
    {
        printf("cyw43_arch_init failed, err=%d", ret);
    }
    else
    {
        printf("cyw43_arch_init success\n");
        cyw43_arch_enable_sta_mode();
    }
}
