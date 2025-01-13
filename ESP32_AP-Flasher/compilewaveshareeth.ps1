#C6                                   WAVESHARE ETH
#GND
#3V
#RST EN       FLASHER_AP_RESET        IO21
#2            FLASHER_AP_TXD          IO16
#3            FLASHER_AP_RXD          IO0
#9   GPIO9    FLASHER_DEBUG_PROG      IO17

#TX  GPIO16   FLASHER_DEBUG_TXD       IO18
#RX  GPIO17   FLASHER_DEBUG_RXD       IO15

#	-D FLASHER_AP_RESET=21
#	-D FLASHER_AP_TXD=16
#	-D FLASHER_AP_RXD=0
#	-D FLASHER_DEBUG_PROG=17
#	-D FLASHER_DEBUG_TXD=18
#	-D FLASHER_DEBUG_RXD=15

# cd ESP32_AP-Flasher

# export PLATFORMIO_BUILD_FLAGS="-D BUILD_VERSION=${{ github.ref_name }} -D SHA=$GITHUB_SHA"

python gzip_wwwfiles.py

&(Join-Path $env:USERPROFILE '\.platformio\penv\Scripts\pio') run --environment ESP32_S3_16_8_WAVESHARE_ETH_AP

&(Join-Path $env:USERPROFILE '\.platformio\penv\Scripts\pio') run --target buildfs --environment ESP32_S3_16_8_WAVESHARE_ETH_AP

# mkdir ESP32_S3_16_8_WAVESHARE_ETH_AP

copy "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin" ESP32_S3_16_8_WAVESHARE_ETH_AP\boot_app0.bin

copy .pio\build\ESP32_S3_16_8_WAVESHARE_ETH_AP\firmware.bin ESP32_S3_16_8_WAVESHARE_ETH_AP\firmware.bin

copy .pio\build\ESP32_S3_16_8_WAVESHARE_ETH_AP\bootloader.bin ESP32_S3_16_8_WAVESHARE_ETH_AP\bootloader.bin

copy .pio\build\ESP32_S3_16_8_WAVESHARE_ETH_AP\partitions.bin ESP32_S3_16_8_WAVESHARE_ETH_AP\partitions.bin

copy .pio\build\ESP32_S3_16_8_WAVESHARE_ETH_AP\littlefs.bin ESP32_S3_16_8_WAVESHARE_ETH_AP\littlefs.bin

copy ESP32_S3_16_8_WAVESHARE_ETH_AP\firmware.bin espbinaries\ESP32_S3_16_8_WAVESHARE_ETH_AP.bin

copy ESP32_S3_16_8_WAVESHARE_ETH_AP\merged-firmware.bin espbinaries\ESP32_S3_16_8_WAVESHARE_ETH_AP_full.bin

cd ESP32_S3_16_8_WAVESHARE_ETH_AP

#python -m esptool                         --chip esp32-s3 merge_bin -o merged-firmware.bin --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin 0x00910000 littlefs.bin

#python -m esptool -p COM3 -b 460800 --before default_reset --after hard_reset --chip esp32s3  write_flash --flash_mode dio --flash_size detect 0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin 0x00910000 littlefs.bin

cd ..


