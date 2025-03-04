
#| Function                           | ESP32H2 Pin           | ESP32S3 Pin   
#| :--------------------------------: | :-------------------: |  :------------------
#| Activate The BOOT Mode Of ESP32H2  | ESP32H2_IO9           | ESP32S3_IO33 
#| Reset ESP32H2                      | ESP32H2_Pin Number 8  | ESP32S3_IO34 
#| Uart                               | ESP32H2_TX_IO24       | ESP32S3_RX_IO48
#| Uart                               | ESP32H2_RX_IO23       | ESP32S3_TX_IO47

&(Join-Path $env:USERPROFILE '\esp\v5.3.2\esp-idf\export.ps1')

idf.py fullclean

idf.py build

#python -m esptool -p COM11 -b 460800 --before default_reset --after hard_reset --chip esp32h2  write_flash --flash_mode dio --flash_size detect --flash_freq 48m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\OpenEPaperLink_esp32_H2.bin


#idf.py -p COM8 flash


#Chip is ESP32-H2 (revision v0.1)
#Features: BLE, IEEE802.15.4
#Crystal is 32MHz
#MAC: 74:4d:bd:ff:fe:63:84:e8
#BASE MAC: 74:4d:bd:63:84:e8
#MAC_EXT: ff:fe
#Uploading stub...
#Running stub...
#Stub running...
#Changing baud rate to 460800
#Changed.
#Configuring flash size...
#Auto-detected Flash size: 4MB
#Flash will be erased from 0x00000000 to 0x00005fff...
#Flash will be erased from 0x00008000 to 0x00008fff...
#Flash will be erased from 0x00010000 to 0x0004dfff...
#Compressed 22080 bytes to 13385...
#Wrote 22080 bytes (13385 compressed) at 0x00000000 in 0.3 seconds (effective 511.5 kbit/s)...
#Hash of data verified.
#Compressed 3072 bytes to 104...
#Wrote 3072 bytes (104 compressed) at 0x00008000 in 0.1 seconds (effective 309.1 kbit/s)...
#Hash of data verified.
#Compressed 253952 bytes to 133359...
#Wrote 253952 bytes (133359 compressed) at 0x00010000 in 1.9 seconds (effective 1098.1 kbit/s)...
#Hash of data verified.
