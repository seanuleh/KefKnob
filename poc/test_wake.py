#!/usr/bin/env python3
"""
Test script to verify if KEF LSX II can be woken via HTTP API.

Usage:
1. Put your KEF speaker in standby using the physical remote
2. Wait 30 seconds to ensure it's fully in standby
3. Run this script: python3 test_wake.py
4. Check if the speaker wakes up
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
    except requests.exceptions.RequestException as e:
        return None


def send_power_on():
    """Send power on command via HTTP API."""
    payload = {
        "path": "settings:/kef/play/physicalSource",
        "roles": "value",
        "value": '{"type":"kefPhysicalSource","kefPhysicalSource":"powerOn"}'
    }

    try:
        response = requests.get(f"{BASE_URL}/setData", params=payload, timeout=5)
        response.raise_for_status()
        return True
    except requests.exceptions.RequestException as e:
        print(f"❌ Error sending power on command: {e}")
        return False


def main():
    print("=" * 60)
    print("KEF LSX II Wake Test")
    print("=" * 60)
    print(f"\nSpeaker IP: {SPEAKER_IP}\n")

    # Step 1: Check initial status
    print("1️⃣  Checking initial speaker status...")
    initial_status = check_speaker_status()

    if initial_status is None:
        print("   ⚠️  Cannot reach speaker - is it on the network?")
        print("   ℹ️  This might mean:")
        print("      - Speaker is in deep standby (network off)")
        print("      - Wrong IP address")
        print("      - Network issue")
        print("\n   Try manually waking with remote first to verify IP.")
        return

    print(f"   ✅ Speaker is reachable. Status: {initial_status}")

    if initial_status == "powerOn":
        print("\n   ⚠️  Speaker is already powered on!")
        print("   📋 Test procedure:")
        print("      1. Use physical remote to put speaker in standby")
        print("      2. Wait 30-60 seconds")
        print("      3. Run this script again")
        return

    # Step 2: Attempt to wake
    print("\n2️⃣  Attempting to wake speaker via HTTP API...")
    time.sleep(1)

    if not send_power_on():
        print("   ❌ Failed to send command")
        return

    print("   ✅ Command sent successfully")

    # Step 3: Wait and verify
    print("\n3️⃣  Waiting 3 seconds for speaker to respond...")
    time.sleep(3)

    print("\n4️⃣  Checking if speaker woke up...")
    final_status = check_speaker_status()

    if final_status is None:
        print("   ❌ Speaker not responding (might have deeper standby)")
        print("\n" + "=" * 60)
        print("❌ RESULT: HTTP API CANNOT wake from this standby state")
        print("=" * 60)
        print("\nThis means your DeskKnob may not be able to wake the")
        print("speaker from standby like the physical remote can.")
        return

    if final_status == "powerOn":
        print("   ✅ Speaker is now powered on!")
        print("\n" + "=" * 60)
        print("✅ SUCCESS: HTTP API CAN wake the speaker!")
        print("=" * 60)
        print("\nYour DeskKnob will be able to wake the KEF LSX II")
        print("from standby just like the remote does.")
    else:
        print(f"   ⚠️  Speaker status is: {final_status}")
        print("\n" + "=" * 60)
        print("❓ UNCLEAR: Speaker responded but didn't fully wake")
        print("=" * 60)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nTest cancelled by user.")
    except Exception as e:
        print(f"\n❌ Unexpected error: {e}")
