"""v8 인원별 틱 처리 시간과 AOI 켬/끔 전송량을 측정한다.

사용법: python measure_v8_scaling.py <v8_server.exe> <v8_load_bot.exe> [--counts 500,1000,2000] [--seconds 15]
                                     [--aoi-off-counts 500] [--rounds 1]

각 측정마다 서버를 새로 띄우고, 부하 봇이 도는 동안 서버의 1초 간격 [Status]를 모아
틱 처리 시간(평균/최대), 늦은 틱, 틱당 송신 바이트를 정리한다. AOI 끔은 V8_CELL_SIZE를 월드 크기로 둔 설정이다.
"""

import argparse
import os
import re
import subprocess
import threading


def run_once(server_path, bot_path, clients, seconds, cell_size):
    env = dict(os.environ, V8_LOG_LEVEL="info", V8_STATUS_INTERVAL_MS="1000",
               V8_CELL_SIZE=str(cell_size), V8_WORLD_SIZE="1000")
    server = subprocess.Popen([server_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env=env, text=True)
    lines = []
    reader = threading.Thread(target=lambda: lines.extend(iter(server.stdout.readline, "")), daemon=True)
    reader.start()
    try:
        bot = subprocess.run([bot_path, "--clients", str(clients), "--threads", "8", "--seconds", str(seconds),
                              "--move-hz", "2"], capture_output=True, text=True, timeout=seconds + 120)
    finally:
        server.stdin.write("\n")
        server.stdin.flush()
        server.wait(timeout=60)
        reader.join(timeout=5)

    # 봇 전원이 들어와 움직이는 구간의 [Status]만 쓴다. 첫 구간은 입장 중일 수 있어 버린다.
    samples = []
    for line in lines:
        if not line.startswith("[Status]"):
            continue
        fields = dict(re.findall(r"(\w+)=([\d.]+)", line))
        if int(fields.get("players", 0)) == clients and float(fields.get("bytes_per_tick", 0)) > 0:
            samples.append(fields)
    samples = samples[1:-1] if len(samples) > 3 else samples
    if not samples:
        raise RuntimeError("no status samples\n" + bot.stdout + "".join(lines[-20:]))
    avg = sum(float(s["tick_avg_ms"]) for s in samples) / len(samples)
    worst = max(float(s["tick_max_ms"]) for s in samples)
    bytes_per_tick = sum(float(s["bytes_per_tick"]) for s in samples) / len(samples)
    last = samples[-1]
    bot_line = next((l for l in bot.stdout.splitlines() if "recv_mb_per_s" in l), "")
    verify_line = next((l for l in bot.stdout.splitlines() if "verify" in l), "")
    result = next((l for l in bot.stdout.splitlines() if "RESULT" in l), "RESULT ?")
    return {
        "clients": clients, "cell": cell_size, "tick_avg_ms": avg, "tick_max_ms": worst,
        "late": int(last["late_ticks"]), "skipped": int(last["skipped_ticks"]), "ticks": int(last["ticks"]),
        "kb_per_tick": bytes_per_tick / 1024, "max_cell": int(last["max_cell_population"]),
        "bot": bot_line, "verify": verify_line, "result": result.split()[-1],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("bot")
    parser.add_argument("--counts", default="500,1000,2000")
    parser.add_argument("--aoi-off-counts", default="500")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--rounds", type=int, default=1)
    args = parser.parse_args()

    plans = [(int(c), 100) for c in args.counts.split(",") if c]
    plans += [(int(c), 1000) for c in args.aoi_off_counts.split(",") if c]
    for _ in range(args.rounds):
        for clients, cell in plans:
            r = run_once(args.server, args.bot, clients, args.seconds, cell)
            print(f"clients={r['clients']} aoi={'on' if r['cell'] < 1000 else 'off'} "
                  f"tick_avg_ms={r['tick_avg_ms']:.3f} tick_max_ms={r['tick_max_ms']:.3f} "
                  f"late={r['late']}/{r['ticks']} skipped={r['skipped']} kb_per_tick={r['kb_per_tick']:.1f} "
                  f"max_cell={r['max_cell']} bot={r['result']}")
            print("   " + r["bot"])
            print("   " + r["verify"])


if __name__ == "__main__":
    main()
