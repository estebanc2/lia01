# Medidor de latencias con ESP32

Firmware para **ESP32 / ESP32-C3** (ESP-IDF 5.5) que mide, con resolución de microsegundos, el tiempo entre una **marca** y dos estímulos, uno **visual** y uno **auditivo**. Los resultados se muestran en una **página web servida por el propio microcontrolador**: tabla de experimentos con hora, y estadísticas (media, desviación estándar, mínimo y máximo). No requiere instalar nada en la computadora o el teléfono: alcanza con un navegador.

> **Estado:** en desarrollo. Ver [Estado del proyecto](#estado-del-proyecto).

## Características

- **Medición por interrupciones de GPIO** con marca de tiempo de 1 µs (`esp_timer`), independiente de la carga de WiFi.
- **Delta con signo:** el experimento comienza con el primer flanco que aparezca, sea la marca o un estímulo. Si el estímulo llega antes que la marca, el tiempo resulta negativo.
- **Tabla de experimentos** con la hora real de cada uno, y **resumen estadístico** calculado en el equipo (sobrevive a recargar la página).
- **Unidades automáticas:** valores menores a 1 ms en µs, el resto en ms.
- **Configurable desde la página:** separación mínima entre experimentos, duración máxima y sentido del flanco de cada señal. Se guarda en memoria no volátil.
- **Botón Limpiar** para empezar una serie nueva.
- **Configuración WiFi sin programar nada:** el equipo crea su propia red (`Medidor-XXXX`) con un portal cautivo en español, con lista de redes y mensajes de error claros (clave incorrecta, red de 5 GHz, etc.).
- **Acceso por nombre:** una vez en tu red, se abre en `http://miesp.local` (mDNS).
- **Un solo usuario a la vez, en red local.** No hay servidor externo ni nube.

## Cómo funciona

```
 primer flanco de cualquiera de las tres señales ──► abre el experimento
 cada señal guarda su primer flanco (marca, visual, auditivo)
 se cierra cuando llegaron las tres, o al cumplirse la duración máxima

 Δ visual   = t_visual   − t_marca
 Δ auditivo = t_auditivo − t_marca        (negativo si la marca llegó después)
```

- Después del primer flanco de cada señal, los siguientes (rebotes, flancos lentos, oscilaciones de un tono) se ignoran.
- Para que una señal pueda **iniciar** un experimento nuevo debe llevar al menos la **separación mínima** (100 ms por defecto) sin flancos. Esto descarta rebotes y tonos largos sin bloquear respuestas rápidas.
- Si falta la marca, la fila se informa como «sin marca»; si falta un estímulo, como «sin señal».
- Las marcas de tiempo se toman dentro de la rutina de interrupción; el envío por WiFi ocurre después y no agrega error a la medida.

## Hardware

| Señal | ESP32 (sugerido) | ESP32-C3 (sugerido) |
|---|---|---|
| Marca | GPIO 25 | GPIO 4 |
| Estímulo visual | GPIO 26 | GPIO 5 |
| Estímulo auditivo | GPIO 27 | GPIO 6 |

Los pines se cambian al inicio de `main/measure.c` (`PIN_MARCA`, `PIN_VIS`, `PIN_AUD`). En el **ESP32 clásico**, los GPIO 6 a 11 pertenecen a la flash y **no deben usarse**; conviene evitar también los de arranque (0, 2, 5, 12, 15). En el C3 evita GPIO 2, 8 y 9.

El sentido de cada flanco es **de bajada por defecto** (pull-up y sensor que baja la señal al activarse) y se cambia desde la página. El pull-up interno viene habilitado para que el pin no flote durante las pruebas (`PULLUP_INTERNAL`).

**Acondicionamiento de señales (importante):**

- **Visual:** un fotodiodo desnudo entrega microamperios y no alcanza a mover un pin con un pull-up. Es más práctico un **fototransistor** con pull-up de 10 kΩ a 100 kΩ, o un módulo con comparador (por ejemplo LM393).
- **Auditivo:** un micrófono entrega milivoltios alternos; requiere **amplificador y comparador** (o un módulo de sonido con salida digital) antes del GPIO.
- La incertidumbre de la medida suele estar dominada por el tiempo de respuesta de los sensores y su acondicionamiento, más que por el firmware. Conviene calibrar con una referencia conocida. El sonido recorre aproximadamente 34 cm por milisegundo.

**Alimentación:** la calibración del WiFi genera picos de corriente. Con un cable USB débil o una fuente justa puede dispararse el detector de *brownout* y el equipo se reinicia en bucle. Usa un cable corto y de calidad, y un capacitor electrolítico de 100 a 470 µF (más uno cerámico de 10 µF) entre 3V3 y GND cerca del módulo.

## Estructura

```
main/
├── main.c            # punto de entrada: arranca la medición y el WiFi
├── wifi_manager.c/.h # conexión WiFi, portal cautivo, DNS, reintentos, reset de credenciales
├── measure.c/.h      # interrupciones, medición, estadísticas, HTTP + WebSocket
├── portal.html       # página de configuración WiFi (embebida en el firmware)
├── app.html          # página de resultados y configuración (embebida en el firmware)
├── idf_component.yml # dependencia: espressif/mdns
└── CMakeLists.txt
sdkconfig.defaults    # WebSocket, flash de 4 MB y otros ajustes
```

## Compilar y cargar

Requisitos: **ESP-IDF 5.5** instalado y activado, y una placa ESP32 o ESP32-C3.

1. Clona el repositorio y entra a la carpeta del proyecto:
   ```
   git clone https://github.com/estebanc2/lia01.git
   cd lia01
   ```
2. Elige el chip: `idf.py set-target esp32` (o `esp32c3`). Ten en cuenta que esto regenera el `sdkconfig`; por eso el soporte de WebSocket se activa desde `sdkconfig.defaults`.
3. Revisa los pines en `main/measure.c` y `RESET_GPIO` en `main/wifi_manager.c` según tu placa.
4. `sdkconfig.defaults` asume **4 MB de flash**; si tu placa tiene otro tamaño, cámbialo en `idf.py menuconfig` → *Serial flasher config → Flash size*.
5. Compila y carga:
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

Las credenciales quedan guardadas. Para borrarlas y volver a ver el portal: **mantén pulsado el pin de reset (BOOT, GPIO 0 en ESP32 y GPIO 9 en C3) durante 2 s dentro de los primeros 10 s tras encender**. No lo mantengas pulsado *al* encender, porque entra en modo de descarga. En placas sin botón, cambia `RESET_GPIO` por otro pin libre y conéctalo a GND, o usa `idf.py erase-flash`.

### Medir

Abre `http://miesp.local`. Cada experimento aparece en la tabla con su hora; el resumen se actualiza solo. **Limpiar** borra la tabla y las estadísticas. La sección **Configuración** permite ajustar la separación mínima, la duración máxima y el flanco de cada señal.

La **hora** de cada experimento la toma el equipo del reloj del navegador, que se le envía al abrir la página.

### Probar sin sensores

Con un cable a GND, toca los tres pines (marca, visual y auditivo) en cualquier orden, con unos milisegundos de diferencia. Debe aparecer un experimento en la tabla; si tocas la marca después de un estímulo, el tiempo sale negativo. Si no tocas alguna señal, a los 2 s la fila indica «sin marca» o «sin señal».

## Configuración

Desde la página (se guarda en memoria no volátil):

| Ajuste | Por defecto | Rango |
|---|---|---|
| Separación mínima entre experimentos | 100 ms | 1 a 10 000 ms |
| Duración máxima de un experimento | 2000 ms | 10 a 60 000 ms |
| Flanco de marca, visual y auditivo | bajada | bajada o subida |

En el código: `PIN_MARCA`, `PIN_VIS`, `PIN_AUD`, `PULLUP_INTERNAL`, `DEF_GAP_MS`, `DEF_MAX_MS` y `HIST_N` (experimentos recordados por el equipo) en `main/measure.c`; `RESET_GPIO` y `PORTAL_LINGER_MS` (cuánto sigue encendido el portal tras conectar) en `main/wifi_manager.c`.

## Interfaz HTTP

| Ruta | Descripción |
|---|---|
| `GET /` | página de resultados y configuración |
| `GET /state` | configuración, últimos `HIST_N` experimentos y estadísticas |
| `POST /config` | cambia la configuración. Cuerpo: `{"gap":100,"max":2000,"r":[0,0,0]}` (`r`: 0 = bajada, 1 = subida, en orden marca, visual, auditivo) |
| `POST /clear` | borra el historial y las estadísticas |
| `POST /time` | fija la hora real. Cuerpo: `{"ms": <milisegundos desde 1970>}` |
| `WS /ws` | un mensaje por cada experimento terminado |

Formato de cada experimento (`GET /state` y `WS /ws`):

```json
{"n": 12, "m": 1, "v": -350, "a": 1204, "ts": 1760000000000}
```

`v` y `a` son los Δ en **microsegundos** (`null` si no hubo señal), `m` indica si llegó la marca, y `ts` es la hora en milisegundos desde 1970 (`null` si el equipo aún no recibió la hora). Las estadísticas (`sv` y `sa`: `n`, `mean`, `std`, `min`, `max`, en µs) acompañan a cada mensaje. La desviación estándar es muestral (n−1).

## Limitaciones conocidas

- Pensado para **un usuario a la vez** en red local; el ESP32 admite pocas conexiones simultáneas.
- La comunicación es HTTP/WebSocket **sin cifrado** y el portal de configuración es una red abierta. Es adecuado para uso local y de laboratorio, no para entornos que requieran seguridad.
- Solo redes WiFi de **2.4 GHz**.
- Si un estímulo no responde, el experimento queda abierto hasta la duración máxima y los flancos del siguiente se le asignarían. Mantén la duración máxima por debajo del período entre experimentos.
- El equipo **no tiene reloj propio**: la hora se pierde al reiniciar hasta que una página vuelva a conectarse, y su precisión es de decenas de milisegundos. Los tiempos medidos (Δ) no dependen de este reloj.
- La tabla de la página conserva los últimos 300 experimentos; el equipo recuerda 20 para recargar la página, pero las estadísticas cubren todos los experimentos desde el último *Limpiar*.
- El nombre `miesp.local` está fijo en el código (`MDNS_HOST`), de modo que dos equipos en la misma red entrarían en conflicto.

## Estado del proyecto

- [x] Portal cautivo de configuración WiFi, con reintentos y mensajes de error
- [x] Descubrimiento por mDNS y servidor HTTP
- [x] Medición por interrupciones, tabla, estadísticas y configuración (probado con señales reales)
- [ ] Diseño de la placa con acondicionamiento de señal y alimentación adecuada
- [ ] Nombre mDNS único por equipo

## Licencia

Distribuido bajo la licencia MIT. Ver [LICENSE](LICENSE).

El proyecto usa ESP-IDF y el componente `espressif/mdns`, que se distribuyen con sus propias licencias.
