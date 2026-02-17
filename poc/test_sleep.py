#!/usr/bin/env python3
"""
Test script to put KEF LSX II into standby via HTTP API.

Usage:
    python3 test_sleep.py
"""

import requests
import time

SPEAKER_IP = "192.168.1.217"
BASE_URL = f"http://{SPEAKER_IP}/api"


def check_speaker_status():
    """Check current speaker status."""
    try:
        payload = {
            "path": "settings:/kef/host/speakerStatus",
            "roles": "value"
        }
        response = requests.get(f"{BASE_URL}/getData", params=payload, timeout=3)
        response.raise_for_status()
        status = response.json()[0]["kefSpeakerStatus"]
        return status
    except requests.exceptions.RequestException:
        return None


def send_standby():
    """Send standby command via HTTP API."""
    payload = {
        "path": "settings:/kef/play/physicalSource",
        "roles": "value",
        "value": '{"type":"kefPhysicalSource","kefPhysicalSource":"standby"}'
    }

    try:
        response = requests.get(f"{BASE_URL}/setData", params=payload, timeout=5)
        response.raise_for_status()
        return True
    except requests.exceptions.RequestException as e:
        print(f"❌ Error sending standby command: {e}")
        return False


def main():
    print("=" * 60)
    print("KEF LSX II Sleep Test")
    print("=" * 60)
    print(f"\nSpeaker IP: {SPEAKER_IP}\n")

    # Check initial status
    print("1️⃣  Checking speaker status...")
    initial_status = check_speaker_status()

    if initial_status is None:
        print("   ❌ Cannot reach speaker")
        return

    print(f"   ✅ Speaker status: {initial_status}")

    if initial_status == "standby":
        print("\n   ⚠️  Speaker is already in standby!")
        return

    # Send standby command
    print("\n2️⃣  Sending standby command...")
    time.sleep(1)

    if not send_standby():
        print("   ❌ Failed to send command")
        return

    print("   ✅ Command sent successfully")

    # Wait and verify
    print("\n3️⃣  Waiting 3 seconds...")
    time.sleep(3)

    print("\n4️⃣  Verifying speaker is in standby...")
    final_status = check_speaker_status()

    if final_status == "standby":
        print("   ✅ Speaker is now in standby!")
        print("\n" + "=" * 60)
        print("✅ SUCCESS: HTTP API can put speaker to sleep!")
        print("=" * 60)
    else:
        print(f"   ⚠️  Speaker status: {final_status}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nTest cancelled.")
    except Exception as e:
        print(f"\n❌ Error: {e}")
