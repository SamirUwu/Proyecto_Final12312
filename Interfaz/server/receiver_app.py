import socket
from PyQt6.QtCore import QThread, pyqtSignal
import json

class TcpServer(QThread):
    json_received = pyqtSignal(dict)

    def __init__(self, port=5001):
        super().__init__()
        self.port = port
        self.running = True

    def run(self):
        try:
            print("🔥 TcpServer thread iniciado")

            server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

            print(f"Intentando bind en 0.0.0.0:{self.port}...")
            server.bind(("0.0.0.0", self.port))
            server.listen(5)
            server.settimeout(1.0)
            print(f"🟢 Servidor listo en puerto {self.port} — esperando conexiones")

            while self.running:
                # ── Esperar nueva conexión ────────────────────────────────────
                try:
                    conn, addr = server.accept()
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"💀 Error en accept: {e}")
                    continue

                print(f"🔵 Conectado desde {addr}")
                conn.settimeout(1.0)
                buffer = ""

                # ── Recibir datos del cliente conectado ───────────────────────
                while self.running:
                    try:
                        data = conn.recv(4096)
                        if not data:
                            print(f"🔌 {addr} cerró la conexión — esperando reconexión...")
                            break

                        buffer += data.decode(errors="ignore")

                        # Intentar parsear con \n como delimitador
                        while "\n" in buffer:
                            line, buffer = buffer.split("\n", 1)
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                parsed = json.loads(line)
                                self.json_received.emit(parsed)
                            except json.JSONDecodeError:
                                pass

                        # Si no hay \n, intentar parsear el buffer completo directamente
                        if buffer.strip():
                            try:
                                parsed = json.loads(buffer.strip())
                                self.json_received.emit(parsed)
                                buffer = ""
                            except json.JSONDecodeError:
                                pass  # incompleto, esperar más datos

                    except socket.timeout:
                        continue
                    except ConnectionResetError:
                        print(f"🔌 {addr} se desconectó — esperando reconexión...")
                        break
                    except Exception as e:
                        print(f"💀 Error: {e}")
                        break

                # Cerrar socket del cliente y volver a esperar
                try:
                    conn.close()
                except Exception:
                    pass
                print(f"♻️  Listo para nueva conexión en puerto {self.port}")

            server.close()
            print("🛑 TcpServer detenido")

        except Exception as e:
            print("💀 ERROR fatal en TcpServer:", e)

    def stop(self):
        self.running = False