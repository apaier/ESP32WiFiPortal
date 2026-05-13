# Credential storage and security

Where the library puts WiFi credentials, what protection that does and doesn't give you, and how to opt into real encryption at rest.

## What gets stored

Three string values on every successful association:

- `network` — the SSID
- `password` — the WPA passphrase, plaintext
- `mode` — `"client"` or `"repeater"`

They live in NVS under the namespace `wifi_portal`, written via the Arduino `Preferences` API. The Preferences-backed NVS partition is the same one Arduino-ESP32 uses for WiFi calibration data — it's already in the default ESP32 partition table and already initialised by the WiFi stack.

A successful soft-fail or hard-fail disconnect does not wipe these values; the captive portal re-uses them to pre-fill the form. The chip's own NVS-stored credentials (separate from this library's namespace, written by `esp_wifi_set_config`) **are** cleared on hard-fail via `WiFi.disconnect(false, true)` so the chip stops auto-reassociating with credentials we know are wrong. See [wifi-recovery.md](wifi-recovery.md).

## Threat model

The asset worth protecting here is the WPA passphrase. The library has to be able to decrypt it on its own at boot to associate with the network — no user is present to type a passphrase — so any protection has to be symmetric and the decryption key has to live on the device.

That bounds the realistic threat model:

| Threat                                                  | Protected by default? |
| ------------------------------------------------------- | --------------------- |
| Reading the password by clicking around the filesystem  | Yes — credentials are in NVS, not in a user-visible file. |
| Inspecting a flash dump pulled off the chip             | **No**, unless flash or NVS encryption is enabled (see below). |
| Cloning the device and walking off with someone's WiFi  | **No**, unless flash encryption is enabled. |
| A web client probing the device on the LAN              | The library never serves the password back; the config form's password field is write-only. |

Default NVS storage is just bytes on flash. Anyone with physical access plus an SPI flash reader can dump the partition and read the password. The previous storage location (`/wifi.json` on LittleFS) was no different in that regard — the move to NVS is a stepping stone toward encryption, not an encryption mechanism on its own.

## How to actually encrypt

Two paths, both opt-in at the project level (not something a library can flip on its consumers).

### Option 1 — Flash encryption (classic ESP32, all variants)

ESP32's flash encryption feature XTS-AES-encrypts whole partitions transparently. The key lives in eFuses and is never readable from software. Enabled in the bootloader / sdkconfig; the partition table flags determine which partitions get encrypted.

- Espressif docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/flash-encryption.html
- For NVS specifically, NVS encryption depends on flash encryption being enabled: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_encryption.html

The fuse-burning step is **irreversible** in release mode. Development mode leaves a window to re-flash with new keys; release mode locks the eFuses permanently. Plan accordingly.

### Option 2 — HMAC-based NVS encryption (ESP32-S2 / S3 / C3)

The HMAC peripheral on newer ESP32 variants can derive an NVS encryption key from an HMAC key burned into eFuses, without enabling whole-chip flash encryption. Lower blast radius, same idea.

- Espressif docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_encryption.html
- Same caveat about irreversible fuse burning.

### What the library does either way

Nothing different. Once your project enables flash or NVS encryption at the partition level, the library's `Preferences` calls go through the encrypted path automatically — there are no code changes here. This is the main practical reason credentials moved to NVS in the first place: it puts them on the storage layer that can be encrypted by configuration alone.

## What might come next

Tracked as backlog item #13: a turn-key path from the library side — boot-time detection of whether encryption is actually active, a clear log warning if it isn't, and recommended `partitions.csv` / `sdkconfig.defaults` snippets so the consumer doesn't have to reverse-engineer the docs. The fuse burning still has to be the consumer's decision, but the runtime "are we encrypted right now?" answer should be visible.

## Files

- [src/ESP32WiFiPortal.cpp](../src/ESP32WiFiPortal.cpp) — `ensurePreferences`, `loadAndConnect`, `persistCredentials`.
- [src/ESP32WiFiPortal.h](../src/ESP32WiFiPortal.h) — `_prefs` member and the `wifi_portal` namespace constant.
