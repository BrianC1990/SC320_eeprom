GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "nogit")
BUILD_TIME := $(shell date +"%Y-%m-%d_%H:%M:%S")
VERSION := "v1.0.1-$(GIT_HASH)"

build:
	# 关键：转义宏值的双引号
	gcc -DVERSION="\"$(VERSION)\"" -DBUILD_TIME="\"$(BUILD_TIME)\"" main.c -o eeprom

run:
	./eeprom --version

clean:
	rm ./eeprom