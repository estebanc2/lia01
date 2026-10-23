# Medidor de latencias con ESP32

Firmware para **ESP32 / ESP32-C3** (ESP-IDF 5.5) que mide, con resolución de microsegundos, el tiempo entre una **marca** y dos estímulos, uno **visual** y uno **auditivo**. Está pensado para experimentos sensibles a interferencias de radio (por ejemplo, junto a un EEG): configuras y ves el equipo funcionar desde el teléfono, y durante la captura el **WiFi se apaga**. Al terminar, los datos quedan guardados en flash y se descargan en CSV.

> **Estado:** en desarrollo. Ver [Estado del proyecto](#estado-del-proyecto).

## Cómo se usa

1. **Configurar.** Al encender, el equipo crea la red WiFi abierta `Medidor-XXXX`. Conéctate con el teléfono; se abre la página (o entra a `http://192.168.4.1`). Ajusta la separación mínima, la duración máxima, el flanco de cada señal, el origen de la marca y si se apaga el WiFi durante la captura. Con señales de prueba, la tabla y las estadísticas se ven en vivo.
2. **Iniciar captura.** Borra los datos guardados antes (descárgalos primero) y empieza a guardar cada experimento en flash.
   - Con **«Apagar el WiFi durante la captura»**, el equipo espera a que se desconecte el último dispositivo (2 s) y apaga el radio por completo.
   - Sin esa opción, sigue en línea: la página muestra los resultados en vivo y tiene un botón **Detener captura**.
3. **Capturar.** Un LED hace un destello breve cada 2 s mientras captura.
4. **Terminar.** Pulsa el **botón de fin** (o el reset). Vuelve el WiFi con los datos disponibles.
5. **Descargar.** En la página: tabla, estadísticas (media, desviación estándar, mínimo, máximo) y **Descargar CSV**.

Un corte de energía no pierde lo ya guardado, y el equipo siempre arranca en modo configuración.

## Cómo funciona la medición

```
 primer flanco de cualquiera de las tres señales ──► abre el experimento
 cada señal guarda su primer flanco (marca, visual, auditivo)
 se cierra cuando llegaron las tres, o al cumplirse la duración máxima

 Δ visual   = t_visual   − t_marca
 Δ auditivo = t_auditivo − t_marca        (negativo si la marca llegó después)
```

- Después del primer flanco de cada señal, los siguientes (rebotes, oscilaciones de un tono) se ignoran.
- Una señal debe llevar al menos la **separación mínima** (100 ms por defecto) sin flancos para poder **iniciar** un experimento nuevo.
- Si falta la marca, el experimento se informa como «sin marca»; si falta un estímulo, como «sin señal».
- Los instantes se toman con `esp_timer` (1 µs), dentro de las interrupciones de GPIO.
- Valores menores a 1 ms se muestran en µs; el resto, en ms.

## Marca por puerto serie

La marca puede venir del **puerto serie USB** de la placa (el conversor USB-serie con el que se carga el firmware, conectado al UART0), a 115 200 baudios. Cada llegada de bytes cuenta como un flanco de la marca; si la PC envía varios bytes seguidos, solo el primero define la marca.

- El instante registrado es el de llegada del primer byte completo (unos 87 µs a 115 200 baudios) más la latencia de la tarea. Ese desfase es pequeño y casi constante. El retardo y la variación del USB y del sistema operativo **quedan incluidos en el Δ medido**.
- **Abre el puerto en la PC *antes* de pulsar «Iniciar captura».** En las placas de desarrollo, los pines DTR y RTS del conversor pueden reiniciar el chip al abrir el puerto. Con pySerial: `s = serial.Serial(); s.port = PUERTO; s.baudrate = 115200; s.dtr = False; s.rts = False; s.open()`.
- Mientras el programa del experimento usa el puerto, no puede estar abierto el monitor serie.
- También se puede usar un **pin** como marca, y el origen se elige en la configuración.

## Hardware

Los pines se eligen según el chip en `main/pins.h`:

| Señal | ESP32 | ESP32-C3 |
|---|---|---|
| Marca (si es por pin) | GPIO 25 | GPIO 4 |
| Estímulo visual | GPIO 26 | GPIO 5 |
| Estímulo auditivo | GPIO 27 | GPIO 6 |
| LED de captura | GPIO 2 | GPIO 7 |
| Botón de fin | GPIO 0 (BOOT) | GPIO 9 (BOOT) |

En el **ESP32 clásico**, los GPIO 6 a 11 pertenecen a la flash y no deben usarse. Por defecto las entradas son de **bajada con pull-up interno** (`PULLUP_INTERNAL` en `measure.c`); el flanco de cada señal se cambia desde la página.

**Acondicionamiento de señales:**

- **Visual:** un fotodiodo desnudo entrega microamperios y no alcanza para un pull-up. Es más práctico un **fototransistor** con pull-up de 10 kΩ a 100 kΩ, o un módulo con comparador (por ejemplo LM393).
- **Auditivo:** un micrófono entrega milivoltios alternos; requiere **amplificador y comparador** (o un módulo de sonido con salida digital) antes del GPIO.
- Si el LED de estado está cerca del sensor visual, colócalo en una caja o pantalla opaca.

## Estructura

```
main/
├── main.c            # punto de entrada
├── pins.h            # pines según el chip
├── measure.c/.h      # interrupciones, medición, estadísticas, almacenamiento en flash, HTTP + WebSocket
├── serial_mark.c     # marca por el puerto serie
├── net.c/.h          # punto de acceso WiFi, DNS de portal cautivo y servidor web
├── session.c/.h      # estados (configuración / espera / captura), LED, botón de fin
├── app.html          # página (embebida en el firmware)
└── CMakeLists.txt
partitions.csv        # partición "datos" de 256 KB
sdkconfig.defaults
```

## Compilar y cargar

Requisitos: **ESP-IDF 5.5** instalado y activado, y una placa ESP32 o ESP32-C3 con **4 MB de flash**.

1. Clona el repositorio:
   ```
   git clone https://github.com/estebanc2/lia01.git
   cd lia01
   ```
2. Elige el chip: `idf.py set-target esp32` (o `esp32c3`). Esto regenera el `sdkconfig`; los ajustes del proyecto están en `sdkconfig.defaults` (WebSocket, 4 MB de flash y la tabla de particiones propia).
3. Revisa los pines en `main/pins.h` según tu placa.
4. Compila y carga: `idf.py build flash monitor`.

## Interfaz HTTP

| Ruta | Descripción |
|---|---|
| `GET /` | página |
| `GET /state` | modo, datos guardados, configuración, últimos experimentos y estadísticas |
| `POST /config` | `{"gap":100,"max":2000,"r":[0,0,0],"wifi_off":1,"src":1}` (`r`: 0 = bajada, 1 = subida, en orden marca, visual, auditivo) |
| `POST /start` | borra lo guardado y empieza la captura |
| `POST /stop` | termina la captura |
| `POST /clear` | vacía la tabla y, fuera de captura, borra lo guardado |
| `POST /time` | `{"ms": <milisegundos desde 1970>}`; la página lo envía al cargar |
| `GET /csv` | datos guardados |
| `WS /ws` | un mensaje por experimento terminado |

Cada experimento: `{"n":12,"m":1,"v":-350,"a":1204,"ts":1760000000000}`, con `v` y `a` en **µs** (`null` si no hubo señal), `m` si llegó la marca y `ts` en ms desde 1970.

**CSV:** `n, epoch_ms, hora_utc, marca, visual_us, auditivo_us`. Los campos vacíos indican que no hubo señal o que el equipo no tenía la hora.

## Limitaciones conocidas

- Un usuario a la vez y red local. El portal es una red abierta (puedes fijar una clave WPA2 con `AP_PASS` en `net.c`), sin cifrado.
- Capacidad: unos 10 900 experimentos por captura; al llenarse, deja de guardar.
- Si un estímulo no responde, el experimento queda abierto hasta la duración máxima, y los flancos del siguiente se le asignarían. Mantén la duración máxima por debajo del período entre experimentos.
- El equipo no tiene reloj propio: la hora la fija la página al cargar (precisión de decenas de ms). Los Δ no dependen de ella.
- Con el WiFi apagado solo se termina con el botón o el reset; el equipo no avisa por radio.

## Estado del proyecto

- [x] Medición por interrupciones, estadísticas y configuración, probado con señales reales
- [ ] Validación del flujo nuevo: punto de acceso, captura en flash, CSV y apagado del WiFi
- [ ] Validación de la marca por puerto serie
- [ ] Diseño de la placa con acondicionamiento de señal

## Licencia

Distribuido bajo la licencia MIT. Ver [LICENSE](LICENSE).

El proyecto usa ESP-IDF, que se distribuye con su propia licencia.
