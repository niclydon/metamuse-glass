# Firmware blobs

This project does not redistribute firmware. The BCM4330 radio needs three blobs at runtime, all extracted from Google's official XE24 factory image (`signed-glass_1-img-5585826.zip`, from Google's Glass system-image download page) — the same image that is this project's recovery floor. The build pipeline extracts them from `system.img`; verify what you extracted against these SHA-256 hashes:

| file | SHA-256 | role |
| --- | --- | --- |
| `fw_bcmdhd.bin` | `c3f7c26a04b348431a4c1f569a013d516612977821d4a36d2416fe218d30c147` | BCM4330 Wi-Fi firmware |
| `bcmdhd.cal` | `435e4ab26aa159525f519671fa1a391595ea8fcaa6bd952fceaf0aeb697d2951` | board calibration, used as the Wi-Fi NVRAM |
| `bcm4330.hcd` | `f87a3fc4c946da97d87a72915c0fcdf3594a3ccfc0751636a0ff75a90e7daf38` | BCM4330 Bluetooth patch file |

These hashes match the ones published by the glass-xec-mainline project for the same blobs, which is a useful independent cross-check.

Two further stock components stay on the device side and are never mirrored here: the display-path FPGA bitstream and the audio DSP firmware, both part of Google's factory image and both left exactly as shipped.
