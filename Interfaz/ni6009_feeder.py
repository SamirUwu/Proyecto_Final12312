"""
ni6009_feeder.py
────────────────
Reads audio from a NI USB-6009 analog input (ai0) and writes packets
into a Windows named pipe (\\\\.\\pipe\\ni6009) using the same binary
protocol as the ESP32 sketch, so the C audio engine can consume it via
SIM_MODE 3.

Packet format (matches serial_input.h):
  [0xAA, 0x55, 0xFF, 0x00]  ← 4-byte sync word
  [s0_lo, s0_hi, s1_lo, s1_hi, ...]  ← PACKET_SAMPLES × uint16_t little-endian

The USB-6009 outputs ±10 V on AI (differential) or 0–5 V (RSE).
We normalise to the same 0-4095 ADC range the ESP32 uses so the
existing serial_adc_to_float() conversion works without changes.

Usage:
  pip install nidaqmx pywin32
  python ni6009_feeder.py [--device Dev1] [--channel ai0] [--rate 44100]
"""

import argparse
import struct
import sys
import time
import signal
import win32pipe
import win32file
import pywintypes
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config import PACKET_SAMPLES, PIPE_NAME

SYNC_WORD = bytes([0xAA, 0x55, 0xFF, 0x00])

# ADC emulation range (matches ESP32 12-bit: 0-4095, midpoint 2048)
ADC_BITS   = 12
ADC_MAX    = (1 << ADC_BITS) - 1   # 4095
ADC_MID    = ADC_MAX // 2          # 2048

running = True

def signal_handler(sig, frame):
    global running
    print("\n[feeder] stopping...")
    running = False

def volts_to_adc(sample_volts, v_min, v_max):
    """Map a voltage sample to a 12-bit ADC code (0-4095)."""
    normalised = (sample_volts - v_min) / (v_max - v_min)   # 0.0 – 1.0
    normalised = max(0.0, min(1.0, normalised))              # clamp
    return int(normalised * ADC_MAX)

def build_packet(samples_volts, v_min, v_max):
    """Build one binary packet: sync word + PACKET_SAMPLES × uint16 LE."""
    adc_codes = [volts_to_adc(v, v_min, v_max) for v in samples_volts]
    payload   = struct.pack(f"<{PACKET_SAMPLES}H", *adc_codes)
    return SYNC_WORD + payload

def open_pipe():
    """Create the Windows named pipe and wait for the C reader to connect."""
    pipe = win32pipe.CreateNamedPipe(
        PIPE_NAME,
        win32pipe.PIPE_ACCESS_OUTBOUND,
        win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT,
        1, 65536, 65536, 0, None)
    print("[feeder] waiting for C reader to connect...")
    win32pipe.ConnectNamedPipe(pipe, None)
    print("[feeder] pipe connected — streaming audio")
    return pipe

def main():
    global running

    parser = argparse.ArgumentParser(description="NI USB-6009 → named pipe feeder")
    parser.add_argument("--device",  default="Dev1",  help="NI device name (default: Dev1)")
    #cualquier canal de AI0 a AI7
    parser.add_argument("--channel", default="ai0",   help="Analog input channel (default: ai0)")
    parser.add_argument("--rate",    type=int, default=44100, help="Sample rate Hz (default: 44100)")
    parser.add_argument("--mode",    choices=["rse", "diff"], default="rse",
                        help="Terminal config: rse (single-ended 0-5V) or diff (differential ±10V)")
    args = parser.parse_args()

    try:
        import nidaqmx
        from nidaqmx.constants import TerminalConfiguration, AcquisitionType
    except ImportError:
        print("[feeder] ERROR: nidaqmx not installed. Run:  pip install nidaqmx")
        sys.exit(1)

    signal.signal(signal.SIGINT,  signal_handler)
    signal.signal(signal.SIGBREAK, signal_handler)

    # Voltage range depends on terminal config
    if args.mode == "rse":
        # USB-6009 RSE: 0 – 5 V (unipolar, AC-coupled guitar signal sits ~2.5 V)
        v_min, v_max = 0.0, 5.0
        term_cfg = TerminalConfiguration.RSE
    else:
        # USB-6009 differential: ±10 V (use for balanced sources)
        v_min, v_max = -10.0, 10.0
        term_cfg = TerminalConfiguration.DIFFERENTIAL

    channel_str = f"{args.device}/{args.channel}"
    print(f"[feeder] NI USB-6009 | channel={channel_str} | rate={args.rate} Hz | mode={args.mode.upper()}")
    print(f"         voltage range: {v_min} V – {v_max} V")
    print(f"         packet size:   {PACKET_SAMPLES} samples")

    pipe_handle = open_pipe()
    try:
        with nidaqmx.Task() as task:
            task.ai_channels.add_ai_voltage_chan(
                channel_str,
                terminal_config=term_cfg,
                min_val=v_min,
                max_val=v_max
            )
            task.timing.cfg_samp_clk_timing(
                rate=args.rate,
                sample_mode=AcquisitionType.CONTINUOUS
            )
            
            # CRITICAL: Set read position to newest samples (skip old ones if buffer fills)
            try:
                task.in_stream.read_relative_to = nidaqmx.constants.ReadRelativeTo.NEWEST_SAMPLE
            except:
                pass
            
            task.start()
            print("[feeder] DAQ task running — press Ctrl+C to stop\n")

            total_packets = 0
            t0 = time.time()
            sample_buffer = []
            CHUNK_SIZE = 16  # Smaller chunks, read more frequently
            skipped_packets = 0

            while running:
                try:
                    # Read a tiny chunk with minimal timeout
                    chunk = task.read(
                        number_of_samples_per_channel=CHUNK_SIZE,
                        timeout=0.5  # Very short timeout
                    )
                    sample_buffer.extend(chunk)
                    
                    # Once we have a full packet, send it
                    if len(sample_buffer) >= PACKET_SAMPLES:
                        samples = sample_buffer[:PACKET_SAMPLES]
                        sample_buffer = sample_buffer[PACKET_SAMPLES:]
                        
                        packet = build_packet(samples, v_min, v_max)
                        
                        try:
                            win32file.WriteFile(pipe_handle, packet)
                        except pywintypes.error:
                            print("[feeder] pipe reader closed — exiting")
                            running = False
                            break
                        
                        total_packets += 1
                        if total_packets % 344 == 0:   # ~1 s at 44100/128
                            elapsed = time.time() - t0
                            pps = total_packets / elapsed
                            print(f"[feeder] {total_packets} packets | {pps:.1f} pkt/s | "
                                  f"last sample: {samples[-1]:.4f} V | skipped: {skipped_packets}")
                
                except nidaqmx.errors.DaqReadError as read_err:
                    # Buffer overflow - skip ahead
                    skipped_packets += 1
                    if skipped_packets % 10 == 0:
                        print(f"[feeder] WARNING: {skipped_packets} buffer overflows")
                    # Just continue - we'll get the next chunk
                    time.sleep(0.001)
                except Exception as e:
                    print(f"[feeder] Fatal error: {type(e).__name__}: {e}")
                    break

    except nidaqmx.errors.DaqError as e:
        print(f"[feeder] DAQmx fatal error: {e}")
    finally:
        win32file.CloseHandle(pipe_handle)
        print("[feeder] done")

if __name__ == "__main__":
    main()
