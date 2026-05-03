import socket
from PyQt6.QtCore import QThread, pyqtSignal
import json

class TcpServer(QThread):
    json_received = pyqtSignal(dict)

    def __init__(self, port=5000):
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

            print("✅ Bind exitoso")
            server.listen(5)
            server.settimeout(1.0)

            print(f"🟢 Servidor listo en puerto {self.port}")

            while self.running:
                try:
                    conn, addr = server.accept()
                except socket.timeout:
                    continue

                print(f"🔵 Conectado desde {addr}")
                conn.settimeout(1.0)

                buffer = ""

                while self.running:
                    try:
                        data = conn.recv(4096)
                        if not data:
                            break

                        buffer += data.decode(errors="ignore")

                        while "\n" in buffer:
                            line, buffer = buffer.split("\n", 1)
                            line = line.strip()

                            if not line:
                                continue

                            try:
                                parsed = json.loads(line)
                                print("📦 JSON recibido:", parsed)
                                self.json_received.emit(parsed)

                            except json.JSONDecodeError as e:
                                print("⚠️ Error JSON:", e)

                    except socket.timeout:
                        continue
                    except Exception as e:
                        print(f"💀 Error recibiendo datos: {e}")
                        break

                conn.close()
                print("🔌 Cliente desconectado")

            server.close()

        except Exception as e:
            print("💀 ERROR en TcpServer:", e)

    def stop(self):
        self.running = False