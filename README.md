# Medidor de latencias visual y auditiva con ESP32

Firmware para **ESP32 / ESP32-C3** (ESP-IDF 5.5) que mide, en milisegundos, el tiempo entre un **estímulo** y las respuestas de dos sensores, uno **visual** y uno **auditivo**, y muestra los resultados en una **página web servida por el propio microcontrolador**. No requiere instalar nada en la computadora o el teléfono: alcanza con un navegador.

> **Estado:** en desarrollo. El portal de configuración WiFi y el servidor web fueron probados en hardware; la medición por interrupciones y la página de resultados están escritas pero **todavía en validación**. Ver [Estado del proyecto](#estado-del-proyecto).

## Características

- **Configuración WiFi sin programar nada:** el equipo crea su propia red (`Medidor-XXXX`) con un portal cautivo en español, con lista de redes, campo de clave y mensajes de error claros (clave incorrecta, red de 5 GHz, etc.).
- **Credenciales guardadas** en memoria no volátil; si la red guardada deja de existir, el portal vuelve a abrirse solo.
- **Acceso por nombre:** una vez en tu red, se abre en `http://miesp.local` (mDNS).
- **Medición por interrupciones de GPIO** con marca de tiempo de 1 µs (`esp_timer`), independiente de la carga de WiFi.
- **Resultados en vivo** por WebSocket: línea de tiempo con el eje X en milisegundos, valores, promedios e historial de las últimas mediciones.
- **Un solo usuario a la vez, en red local.** No hay servidor externo ni nube.

## Cómo funciona

```
 estímulo (flanco) ──► t0
                         ├──► primer flanco del sensor visual   ─► t_visual   = t1 - t0
                         └──► primer flanco del sensor auditivo ─► t_auditivo = t2 - t0
```

1. Un flanco válido en el pin de **estímulo** abre una medición y guarda `t0`.
2. Cada sensor registra solo **su primer flanco posterior** a `t0`; el resto de flancos (rebotes, flancos lentos, oscilaciones de un tono) se ignoran hasta el siguiente estímulo.
3. Cuando llegan ambos, o pasan 2 s, se publica el resultado. Un sensor sin respuesta se informa como «sin respuesta».
4. El pin de estímulo tiene una **zona muerta de 100 ms** para ignorar sus propios rebotes.

Las marcas de tiempo se toman dentro de la rutina de interrupción; el envío por WiFi ocurre después y no agrega error a la medida.

## Hardware

| Señal | ESP32 (sugerido) | ESP32-C3 (sugerido) |
|---|---|---|
| Estímulo | GPIO 25 | GPIO 4 |
| Sensor visual | GPIO 26 | GPIO 5 |
| Sensor auditivo | GPIO 27 | GPIO 6 |

Los pines se cambian en las primeras líneas de `main/measure.c`. En el **ESP32 clásico**, los GPIO 6 a 11 pertenecen a la flash y **no deben usarse**; conviene evitar también los de arranque (0, 2, 5, 12, 15). En el C3 evita GPIO 2, 8 y 9.

Por defecto se asume lógica con **pull-up y flanco descendente**: el sensor baja la señal al activarse. El pull-up interno viene habilitado para que el pin no flote durante las pruebas (`PULLUP_INTERNAL` en `measure.c`).

**Acondicionamiento de señales (importante):**

- **Visual:** un fotodiodo desnudo entrega microamperios y no alcanza a mover un pin con un pull-up. Es más práctico un **fototransistor** con pull-up de 10 kΩ a 100 kΩ, o un módulo con comparador (por ejemplo LM393).
- **Auditivo:** un micrófono entrega milivoltios alternos; requiere **amplificador y comparador** (o un módulo de sonido con salida digital) antes del GPIO.
- La incertidumbre de la medida suele estar dominada por el tiempo de respuesta de los sensores y su acondicionamiento, más que por el firmware. Conviene calibrar con una referencia conocida. Recuerda que el sonido recorre aproximadamente 34 cm por milisegundo.

**Alimentación:** la calibración del WiFi genera picos de corriente. Con un cable USB débil o una fuente justa puede dispararse el detector de *brownout* y el equipo se reinicia en bucle. Usa un cable corto y de calidad, y un capacitor electrolítico de 100 a 470 µF (más uno cerámico de 10 µF) entre 3V3 y GND cerca del módulo.

## Estructura

```
main/
├── main.c            # punto de entrada: arranca la medición y el WiFi
├── wifi_manager.c/.h # conexión WiFi, portal cautivo, DNS, reintentos, reset de credenciales
├── measure.c/.h      # interrupciones, lógica de medición, HTTP + WebSocket
├── portal.html       # página de configuración WiFi (embebida en el firmware)
├── app.html          # página de resultados (embebida en el firmware)
├── idf_component.yml # dependencia: espressif/mdns
└── CMakeLists.txt
```

## Compilar y cargar

Requisitos: **ESP-IDF 5.5** instalado y activado, y una placa ESP32 o ESP32-C3.

1. Clona el repositorio y entra a la carpeta del proyecto:
   ```
   git clone <URL-DEL-REPOSITORIO>
   cd <carpeta>
   ```
2. Elige el chip: `idf.py set-target esp32` (o `esp32c3`).
3. Revisa los pines en `main/measure.c` y `RESET_GPIO` en `main/wifi_manager.c` según tu placa.
4. Activa el soporte de **WebSocket** del servidor HTTP: `idf.py menuconfig` → *Component config → HTTP Server → WebSocket server support*.
5. Si tu placa tiene 4 MB de flash, configúralo en *Serial flasher config → Flash size*.
6. Compila y carga:
   ```
   idf.py build flash monitor
   ```

El componente `espressif/mdns` se descarga solo en la primera compilación gracias a `idf_component.yml`.

## Uso

### Primera conexión a WiFi

1. Enciende el equipo. Aparece la red WiFi abierta **`Medidor-XXXX`**.
2. Conéctate desde el teléfono o la computadora. Debería abrirse sola la ventana de inicio de sesión; si no, abre `http://192.168.4.1`.
3. Elige tu red (**de 2.4 GHz**), escribe la clave y pulsa **Conectar**.
4. Al terminar, la página indica qué hacer: reconecta el dispositivo a tu WiFi habitual y abre **`http://miesp.local`**. Si tu sistema no resuelve nombres `.local` (a veces ocurre en Android), usa la IP que imprime el monitor serie (`Conectado, IP ...`).

Las credenciales quedan guardadas. Para borrarlas y volver a ver el portal: **mantén pulsado el botón BOOT (GPIO 0 en ESP32) durante 2 s dentro de los primeros 10 s tras encender**. No lo mantengas pulsado *al* encender, porque entra en modo de descarga. En placas sin botón, cambia `RESET_GPIO` por otro pin libre y conéctalo a GND, o usa `idf.py erase-flash`.

### Medir

Abre `http://miesp.local`. La página se actualiza sola con cada nuevo estímulo.

### Probar sin sensores

Con un cable a GND: toca primero el pin de **estímulo** y, unos instantes después, el del **visual** y el del **auditivo**. Debe aparecer la medición en la página. Si tocas solo el estímulo, a los 2 s se informa «sin respuesta» en ambos.

## Configuración

Constantes al inicio de `main/measure.c`:

| Constante | Valor por defecto | Descripción |
|---|---|---|
| `PIN_STIM`, `PIN_VIS`, `PIN_AUD` | según chip | GPIO de cada señal |
| `EDGE` | `GPIO_INTR_NEGEDGE` | flanco activo |
| `PULLUP_INTERNAL` | `1` | habilita el pull-up interno |
| `DEAD_US` | `100000` | zona muerta del estímulo (µs) |
| `TIMEOUT_US` | `2000000` | tiempo máximo de espera de los sensores (µs) |
| `HIST_N` | `20` | mediciones guardadas en el equipo |

En `main/wifi_manager.c`: `RESET_GPIO` (pin de borrado de credenciales) y `PORTAL_LINGER_MS` (cuánto sigue encendido el portal tras conectar, para leer el resultado).

## Interfaz HTTP

| Ruta | Descripción |
|---|---|
| `GET /` | página de resultados |
| `GET /history` | últimas mediciones, JSON: `[{"n":1,"vis":123.4,"aud":150.2}, ...]` |
| `WS /ws` | cada medición nueva, como un mensaje de texto JSON con el mismo formato |

Los tiempos están en milisegundos; `null` significa «sin respuesta».

## Limitaciones conocidas

- Está pensado para **un usuario a la vez** en red local. El ESP32 admite pocas conexiones simultáneas.
- La comunicación es HTTP/WebSocket **sin cifrado** y el portal de configuración es una red abierta. Es adecuado para uso local y de laboratorio, no para entornos que requieran seguridad.
- Solo redes WiFi de **2.4 GHz**.
- Se asume que los sensores responden **después** del estímulo; no se registran tiempos negativos.
- El nombre `miesp.local` está fijo en el código (`MDNS_HOST`), de modo que dos equipos en la misma red entrarían en conflicto.

## Estado del proyecto

- [x] Portal cautivo de configuración WiFi, con reintentos y mensajes de error
- [x] Descubrimiento por mDNS y servidor HTTP
- [ ] Validación en hardware de la medición por interrupciones y la página de resultados
- [ ] Validación con sensores reales (fototransistor, micrófono con comparador)
- [ ] Diseño de la placa con acondicionamiento de señal y alimentación adecuada
- [ ] Nombre mDNS único por equipo

## Licencia

*Pendiente de definir.*
