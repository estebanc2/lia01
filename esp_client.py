#!/usr/bin/env python3
"""
Cliente PC para un ESP32-C3 con firmware ESP-IDF.

Flujo:
  1. Intenta descubrir el ESP por mDNS (miesp.local). Si lo encuentra, salta al paso 4.
  2. Si no, espera a que la PC este conectada al AP de provisioning (PROV_MIESP).
  3. Envia SSID/clave usando esp_prov.py (herramienta oficial de ESP-IDF).
  4. Descubre el ESP en la red, y consulta GET /data una vez por segundo
     mostrando el contador en pantalla.

Dependencias:
    pip install requests zeroconf

Para el provisioning se necesita ESP-IDF instalado (usa tools/esp_prov/esp_prov.py).
Lo mas simple es ejecutar este script desde la terminal de ESP-IDF, o definir
la variable de entorno IDF_PATH / pasar --idf-path.
"""

from __future__ import annotations

import argparse
import getpass
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import requests
from zeroconf import IPVersion, ServiceBrowser, ServiceStateChange, Zeroconf

SOFTAP_HOST = "192.168.4.1"
SOFTAP_PORT = 80


# --------------------------------------------------------------------------- #
# Provisioning
# --------------------------------------------------------------------------- #
def softap_reachable(timeout: float = 2.0) -> bool:
    """True si la PC puede hablar con el AP de provisioning del ESP."""
    try:
        with socket.create_connection((SOFTAP_HOST, SOFTAP_PORT), timeout=timeout):
            return True
    except OSError:
        return False


def find_esp_prov(idf_path: str | None) -> Path:
    idf = idf_path or os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("No encuentro ESP-IDF. Defini IDF_PATH o usa --idf-path.")
    script = Path(idf) / "tools" / "esp_prov" / "esp_prov.py"
    if not script.is_file():
        sys.exit(f"No existe {script}")
    return script


def provision(esp_prov: Path, ssid: str, password: str, pop: str) -> bool:
    """Envia las credenciales WiFi al ESP usando esp_prov.py."""
    cmd = [
        sys.executable, str(esp_prov),
        "--transport", "softap",
        "--service_name", f"{SOFTAP_HOST}:{SOFTAP_PORT}",
        "--sec_ver", "1",
        "--pop", pop,
        "--ssid", ssid,
        "--passphrase", password,
    ]
    print("Enviando credenciales al ESP...")
    result = subprocess.run(cmd)
    return result.returncode == 0


def wait_for_softap() -> None:
    """Pide al usuario que se conecte al AP del ESP y espera a que responda."""
    print()
    print("El ESP no esta en la red. Conecta la PC a la red WiFi 'PROV_MIESP'")
    print("(es abierta) y espera a que el sistema termine de conectarse.")
    while not softap_reachable():
        input("Presiona Enter para reintentar (Ctrl+C para salir)... ")
    print("AP de provisioning detectado.")


# --------------------------------------------------------------------------- #
# Descubrimiento mDNS
# --------------------------------------------------------------------------- #
def discover(hostname: str, timeout: float = 8.0) -> tuple[str, int] | None:
    """Busca '<hostname>.local' anunciando _http._tcp. Devuelve (ip, puerto)."""
    target = f"{hostname}.local".lower()
    found: dict[str, object] = {}
    event = threading.Event()

    def on_change(zeroconf, service_type, name, state_change):
        if state_change is not ServiceStateChange.Added:
            return
        info = zeroconf.get_service_info(service_type, name)
        if info is None or not info.server:
            return
        if info.server.lower().rstrip(".") != target:
            return
        addrs = info.parsed_addresses(IPVersion.V4Only)
        if addrs:
            found["addr"] = (addrs[0], info.port)
            event.set()

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, "_http._tcp.local.", handlers=[on_change])
        event.wait(timeout)
    finally:
        zc.close()
    return found.get("addr")  # type: ignore[return-value]


def discover_forever(hostname: str) -> tuple[str, int]:
    while True:
        print(f"Buscando {hostname}.local en la red...")
        res = discover(hostname)
        if res:
            return res


# --------------------------------------------------------------------------- #
# Lectura de datos
# --------------------------------------------------------------------------- #
def read_counter(base_url: str) -> int:
    r = requests.get(f"{base_url}/data", timeout=2)
    r.raise_for_status()
    return int(r.json()["contador"])


def monitor(hostname: str, interval: float) -> None:
    base_url = None
    fails = 0
    print("Ctrl+C para salir.\n")
    while True:
        if base_url is None:
            ip, port = discover_forever(hostname)
            base_url = f"http://{ip}:{port}"
            print(f"ESP encontrado en {base_url}\n")
        try:
            value = read_counter(base_url)
            fails = 0
            print(f"\rContador: {value:>10}", end="", flush=True)
        except (requests.RequestException, KeyError, ValueError):
            fails += 1
            print("\rSin respuesta del ESP...          ", end="", flush=True)
            if fails >= 3:
                # Puede haber cambiado de IP (DHCP): se vuelve a descubrir.
                print()
                base_url = None
                fails = 0
                continue
        time.sleep(interval)


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(description="Cliente PC para ESP32-C3")
    ap.add_argument("--hostname", default="miesp", help="hostname mDNS del ESP")
    ap.add_argument("--pop", default="abcd1234", help="proof of possession")
    ap.add_argument("--ssid", help="SSID de tu red WiFi (2.4 GHz)")
    ap.add_argument("--password", help="clave WiFi (si se omite, se pregunta)")
    ap.add_argument("--idf-path", help="ruta a ESP-IDF (por defecto $IDF_PATH)")
    ap.add_argument("--interval", type=float, default=1.0, help="segundos entre lecturas")
    args = ap.parse_args()

    try:
        # 1) Quizas el ESP ya esta provisionado y en la red.
        print("Comprobando si el ESP ya esta en la red...")
        if discover(args.hostname, timeout=5.0) is None:
            # 2) Hay que provisionarlo.
            esp_prov = find_esp_prov(args.idf_path)
            ssid = args.ssid or input("SSID de tu red WiFi: ")
            password = args.password or getpass.getpass("Clave WiFi: ")

            wait_for_softap()
            if not provision(esp_prov, ssid, password, args.pop):
                sys.exit("Fallo el provisioning. Revisa SSID/clave/PoP.")

            print()
            print("Provisioning OK. El ESP va a conectarse a tu WiFi.")
            print("Reconecta la PC a esa misma red si no lo hace sola.\n")

        # 3) Descubrir y mostrar el contador.
        monitor(args.hostname, args.interval)
    except KeyboardInterrupt:
        print("\nChau.")


if __name__ == "__main__":
    main()
