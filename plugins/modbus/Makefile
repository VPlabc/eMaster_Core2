CC ?= cc
SDK_ROOT ?= ../../sdk/hsf-plugin-sdk
TARGET ?= plugin.so
PLATFORM ?= linux-x86_64

CPPFLAGS += -I$(SDK_ROOT)/include -DHSF_PLATFORM_TRIPLE=\"$(PLATFORM)\"
CFLAGS += -std=c11 -O2 -fPIC -Wall -Wextra -Wpedantic
LDFLAGS += -shared

.PHONY: all test clean

all: $(TARGET)

$(TARGET): src/modbus_driver_c.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: tests/test_modbus_driver_c
	./tests/test_modbus_driver_c

tests/test_modbus_driver_c: tests/test_modbus_driver_c.c src/modbus_driver_c.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_modbus_driver_c.c

clean:
	rm -f $(TARGET) tests/test_modbus_driver_c
