#pragma once

void net_start(void);     /* WiFi AP "Medidor-XXXX" + DNS + servidor web (http://192.168.4.1) */
void net_stop(void);      /* apaga servidor, DNS y radio */
int  net_clients(void);   /* dispositivos conectados al AP */
