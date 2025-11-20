import socket
import threading
import queue
import time

class PersistentClient:
    def __init__(self, name, host="10.20.26.128", port=5555):
        self.name = name
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.msg_queue = queue.Queue()

    def connect(self):
        """Establish persistent connection."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self.running = True
        threading.Thread(target=self._listen_loop, daemon=True).start()

    def subscribe(self, topic):
        """Subscribe to a topic."""
        cmd = f"SUB {self.name} {topic}\n"
        self.sock.sendall(cmd.encode())

    def publish(self, topic, message):
        """Publish message to a topic."""
        cmd = f"PUB {topic} {message}\n"
        self.sock.sendall(cmd.encode())

    def _listen_loop(self):
        """Listen continuously for server messages."""
        while self.running:
            try:
                data = self.sock.recv(1024)
                if not data:
                    break
                msg = data.decode().strip()
                self.msg_queue.put(msg)
            except Exception:
                break

    def get_message(self, timeout=0.1):
        """Return message if available."""
        try:
            return self.msg_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.running = False
        if self.sock:
            self.sock.close()
