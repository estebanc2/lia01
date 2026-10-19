#pragma once

#define MDNS_HOST "miesp"   /* el equipo se abre en http://miesp.local */

typedef void (*wifi_got_ip_cb_t)(const char *ip);

/* Conecta con las credenciales guardadas; si no hay (o fallan 5 veces),
 * abre el portal de configuracion (AP "Medidor-XXXX").
 * cb se llama UNA vez, cuando ya hay IP y el puerto 80 esta libre. */
void wifi_manager_start(wifi_got_ip_cb_t cb);
