#!/usr/bin/bash

if [[ -d $1 ]]; then
	rm -rf $1/*
	cp -r data/* $1
	rm $1/www/*
	cp -r wwwroot/* $1/www/
	cp ../binaries/ESP32-C6/firmware.json $1
	for f in bootloader partition-table OpenEPaperLink_esp32_C6
	do
		if [[ -e ../ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/build/bootloader/${f}.bin ]]; then
			cp ../ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/build/bootloader/${f}.bin $1
		else
			cp ../binaries/ESP32-C6/${f}.bin $1
		fi
	done
	mkdir $1/current
	echo "[[]]" > $1/current/tagDB.json
	echo "OK"
else
	echo "$1 is not a directory"
	exit 1
fi
