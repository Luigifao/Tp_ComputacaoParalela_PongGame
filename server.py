import asyncio
import json
import os
import random
from aiohttp import web
import aiohttp

# ---------------- Configuracoes do jogo ----------------
WIDTH = 800
HEIGHT = 400
PADDLE_HEIGHT = 100
PADDLE_WIDTH = 10
BALL_SIZE = 10
PADDLE_SPEED = 5
FPS = 60

# ---------------- Estado do jogo ----------------
game_state = {
    "ball": {"x": WIDTH / 2, "y": HEIGHT / 2, "vx": 4, "vy": 4},
    "paddle1": {"y": HEIGHT / 2 - PADDLE_HEIGHT / 2, "target_y": HEIGHT / 2 - PADDLE_HEIGHT / 2},
    "paddle2": {"y": HEIGHT / 2 - PADDLE_HEIGHT / 2, "target_y": HEIGHT / 2 - PADDLE_HEIGHT / 2},
    "score1": 0,
    "score2": 0,
    "running": False,
}

esp32_clients = [None, None]
browser_clients = set()
state_lock = asyncio.Lock()

# Caminho absoluto para o index.html (funciona de qualquer diretorio)
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_PATH = os.path.join(BASE_DIR, "public", "index.html")


# ---------------- Loop de fisica ----------------
async def game_loop():
    while True:
        await asyncio.sleep(1 / FPS)
        if not game_state["running"]:
            continue

        async with state_lock:
            # Movimento suave das raquetes (interpolacao)
            for p in (game_state["paddle1"], game_state["paddle2"]):
                p["y"] += (p["target_y"] - p["y"]) * 0.3
                p["y"] = max(0, min(HEIGHT - PADDLE_HEIGHT, p["y"]))

            ball = game_state["ball"]
            p1 = game_state["paddle1"]
            p2 = game_state["paddle2"]

            ball["x"] += ball["vx"]
            ball["y"] += ball["vy"]

            # Colisao topo/base
            if ball["y"] <= 0 or ball["y"] >= HEIGHT - BALL_SIZE:
                ball["vy"] *= -1

            # Colisao com paddle 1 (esquerda)
            if (ball["x"] <= PADDLE_WIDTH
                    and ball["y"] + BALL_SIZE >= p1["y"]
                    and ball["y"] <= p1["y"] + PADDLE_HEIGHT):
                ball["vx"] *= -1
                ball["x"] = PADDLE_WIDTH

            # Colisao com paddle 2 (direita)
            if (ball["x"] + BALL_SIZE >= WIDTH - PADDLE_WIDTH
                    and ball["y"] + BALL_SIZE >= p2["y"]
                    and ball["y"] <= p2["y"] + PADDLE_HEIGHT):
                ball["vx"] *= -1
                ball["x"] = WIDTH - PADDLE_WIDTH - BALL_SIZE

            # Pontuacao
            if ball["x"] < 0:
                game_state["score2"] += 1
                reset_ball()
            elif ball["x"] > WIDTH:
                game_state["score1"] += 1
                reset_ball()

            msg = json.dumps({"type": "state", **game_state})

        await broadcast_to_browsers(msg)


def reset_ball():
    game_state["ball"] = {
        "x": WIDTH / 2,
        "y": HEIGHT / 2,
        "vx": 4 * random.choice([1, -1]),
        "vy": 4 * random.choice([1, -1]),
    }


async def broadcast_to_browsers(msg):
    if not browser_clients:
        return
    dead = set()
    for client in browser_clients:
        try:
            await client.send_str(msg)
        except Exception:
            dead.add(client)
    for d in dead:
        browser_clients.discard(d)


# ---------------- Handler WebSocket ----------------
async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    player_index = None
    is_browser = False
    print(f"[NOVA CONEXAO] {request.remote}")

    try:
        async for msg in ws:
            if msg.type != aiohttp.WSMsgType.TEXT:
                continue

            raw = msg.data
            print(f"[RECEBIDO] {raw}")

            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                continue

            # ----- Registro -----
            if data.get("type") == "register":
                role = data.get("role")

                if role == "esp32":
                    pid = data.get("player")
                    idx = (pid - 1) if pid in (1, 2) else None
                    if idx is None:
                        if esp32_clients[0] is None:
                            idx = 0
                        elif esp32_clients[1] is None:
                            idx = 1

                    if idx is None:
                        await ws.send_str(json.dumps({
                            "type": "error",
                            "message": "Maximo de ESP32 atingido",
                        }))
                        continue

                    esp32_clients[idx] = ws
                    player_index = idx
                    print(f"[+] ESP32 registrado como Jogador {idx + 1}")
                    await ws.send_str(json.dumps({
                        "type": "registered", "player": idx + 1,
                    }))

                elif role == "browser":
                    browser_clients.add(ws)
                    is_browser = True
                    print("[+] Navegador registrado")
                    async with state_lock:
                        await ws.send_str(json.dumps({
                            "type": "state", **game_state,
                        }))
                continue

            # ----- Movimento do ESP32 (agora com speed -100 a +100) -----
            if data.get("type") == "move" and player_index is not None:
                speed = data.get("speed", 0) / 100.0
                async with state_lock:
                    paddle = (game_state["paddle1"]
                              if player_index == 0
                              else game_state["paddle2"])
                    alvo = paddle["target_y"] + speed * PADDLE_SPEED * 3
                    paddle["target_y"] = max(0, min(HEIGHT - PADDLE_HEIGHT, alvo))

            # ----- Comandos do navegador -----
            if data.get("type") == "start" and is_browser:
                async with state_lock:
                    game_state["running"] = True
                    game_state["score1"] = 0
                    game_state["score2"] = 0
                    game_state["paddle1"]["y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle1"]["target_y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle2"]["y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle2"]["target_y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    reset_ball()
                print("[+] Jogo iniciado")

            if data.get("type") == "reset" and is_browser:
                async with state_lock:
                    game_state["running"] = False
                    game_state["score1"] = 0
                    game_state["score2"] = 0
                    game_state["paddle1"]["y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle1"]["target_y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle2"]["y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    game_state["paddle2"]["target_y"] = HEIGHT / 2 - PADDLE_HEIGHT / 2
                    reset_ball()
                print("[+] Jogo reiniciado")

    except Exception as e:
        print(f"[ERRO] {e}")
    finally:
        if player_index is not None:
            esp32_clients[player_index] = None
            print(f"[-] ESP32 Jogador {player_index + 1} desconectado")
        if is_browser:
            browser_clients.discard(ws)
            print("[-] Navegador desconectado")

    return ws


# ---------------- Handler HTTP ----------------
async def index_handler(request):
    return web.FileResponse(INDEX_PATH)


# ---------------- Inicializacao ----------------
async def main():
    app = web.Application()
    app.router.add_get("/", index_handler)
    app.router.add_get("/ws", websocket_handler)

    asyncio.create_task(game_loop())

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "0.0.0.0", 8080)
    await site.start()

    print("Servidor HTTP + WebSocket rodando em http://0.0.0.0:8080")
    print("  - HTML:      http://SEU_IP:8080/")
    print("  - WebSocket: ws://SEU_IP:8080/ws")

    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())