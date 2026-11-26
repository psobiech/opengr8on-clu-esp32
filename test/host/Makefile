PROTO := ../../components/clu_proto
COMP := ../../components
MAIN := ../../main
# Everything built (test binaries, objects, the Lua library, gcov data, coverage reports) goes here.
OUT := out
LUALIB := $(OUT)/liblua_host.a
$(shell mkdir -p $(OUT))

# make COVERAGE=1 ...: instrument the code under test (gcov); `make coverage` does the whole run.
COVERAGE ?= 0
COV := $(if $(filter 1,$(COVERAGE)),--coverage)
TCC := $(CC) $(COV)
SAN := -fsanitize=address,undefined
# Coverage builds run unoptimised: -O1 merges lines and turns switches into tables gcov cannot attribute.
OPT := $(if $(COV),-O0,-O1)
CFLAGS := -std=c11 -Wall -Wextra -Werror $(OPT) -g $(SAN) -I$(PROTO)/include
# Tests compile against these stubs and headers: rebuild when they change.
STUBS := $(wildcard stubs/*.h stubs/*/*.h)
HDRS := $(wildcard $(COMP)/*/include/*.h $(COMP)/tfbus_drivers/*.h $(MAIN)/*.h)

$(OUT)/test_proto: test_proto.c $(PROTO)/tfbus_frame.c $(PROTO)/clu_keys.c $(PROTO)/tftp_proto.c $(PROTO)/http_proto.c $(HDRS)
	$(TCC) $(CFLAGS) -o $@ $(filter %.c,$^)

# Lua version under test: make LUA=5.5 clean test; LUA_FLOAT=0 uses doubles, LUA32=1 uses 32-bit
# integers and floats.
LUA ?= 5.3
LUA32 ?= 0
LUA_FLOAT ?= 1
LUA_DIR := $(COMP)/lua/src-$(LUA)
LUA_DEFS := $(if $(filter 5.3,$(LUA)),-DLUA_COMPAT_5_2) $(if $(filter 1,$(LUA32)),-DLUA_32BITS=1,$(if $(filter 1,$(LUA_FLOAT)),-DLUA_FLOAT_TYPE=LUA_FLOAT_FLOAT)) -DLUAI_MAXCCALLS=120 -DMAXCCALLS=120 # = components/lua/CMakeLists.txt
LUA_SRCS := $(filter-out %/lua.c %/luac.c,$(wildcard $(LUA_DIR)/*.c))
DRV_SRCS := $(wildcard $(COMP)/tfbus_drivers/*.c)
DRV_INC := -Istubs -I$(COMP)/clu_proto/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include \
           -I$(COMP)/tfbus_master/include -I$(COMP)/tfbus_drivers/include

# The Lua VM itself is not instrumented (third-party, not under test).
$(LUALIB): $(LUA_SRCS)
	rm -rf $@ $(OUT)/lua && mkdir -p $(OUT)/lua
	cd $(OUT)/lua && $(CC) -std=gnu99 -O1 -g $(SAN) $(LUA_DEFS) -DLUA_USE_LINUX -c $(abspath $(LUA_SRCS))
	ar rcs $@ $(OUT)/lua/*.o && rm -rf $(OUT)/lua

$(OUT)/test_drivers: test_drivers.c $(DRV_SRCS) $(PROTO)/tfbus_frame.c $(LUALIB) $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    $(LUA_DEFS) $(DRV_INC) -o $@ test_drivers.c $(DRV_SRCS) $(PROTO)/tfbus_frame.c $(LUALIB) -lm -ldl

MASTER_INC := -Istubs -I$(COMP)/clu_proto/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include \
              -I$(COMP)/tfbus_master/include -I$(COMP)/tfbus_uart/include

$(OUT)/test_master: test_master.c $(COMP)/tfbus_master/tfbus_master.c $(PROTO)/tfbus_frame.c $(LUALIB) $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-but-set-variable $(OPT) -g $(SAN) \
	    $(LUA_DEFS) $(MASTER_INC) -o $@ test_master.c $(PROTO)/tfbus_frame.c $(LUALIB) -lm -ldl

# On-board IO emulation (two IO_MODULE_DIN_8 answering TF-Bus frames) against fake expanders.
$(OUT)/test_localio: test_localio.c $(COMP)/clu_localio/localio.c $(PROTO)/tfbus_frame.c $(PROTO)/tfbus_vmod.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    -Istubs -I$(PROTO)/include -I$(COMP)/clu_localio/include -o $@ $(filter %.c,$^)

# On-board analog inputs / 1-Wire sensors as TF-Bus modules (localio_sensors.c, included by the test).
$(OUT)/test_localio_sensors: test_localio_sensors.c $(COMP)/clu_localio/localio_sensors.c $(PROTO)/tfbus_frame.c $(PROTO)/tfbus_vmod.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    -Istubs -I$(PROTO)/include -I$(COMP)/clu_localio/include -o $@ test_localio_sensors.c $(PROTO)/tfbus_frame.c $(PROTO)/tfbus_vmod.c

# On-board analog inputs / 1-Wire buses hardware glue (clu_localio_sensors.c, included by the test)
# against a fake ADC1, fake 1-Wire buses and a fake localio_sensors.
$(OUT)/test_localio_hw: test_localio_hw.c $(COMP)/clu_localio/clu_localio_sensors.c $(COMP)/clu_localio/localio_onewire_pins.h \
                        $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(PROTO)/include -I$(COMP)/clu_localio/include -I$(COMP)/tfbus_uart/include \
	    -o $@ test_localio_hw.c

# On-board IO board glue (clu_localio.c, included by the test): serial in NVS, PCF8575 pin mapping
# of KinCony F8 and KC868-A8v3, against a fake expander.
LOCALIO_BOARD_DEPS := test_localio_board.c $(COMP)/clu_localio/clu_localio.c $(COMP)/clu_localio/localio.c \
                      $(COMP)/clu_i2c/clu_i2c.c $(PROTO)/tfbus_frame.c $(PROTO)/tfbus_vmod.c fake_nvs.h $(STUBS) $(HDRS)
LOCALIO_BOARD_CC = $(TCC) $(HOST_CFLAGS) -Wno-unused-function -Istubs -I. -I$(PROTO)/include -I$(COMP)/clu_localio/include \
                   -I$(COMP)/clu_crypto/include -I$(COMP)/tfbus_uart/include -I$(COMP)/clu_i2c/include \
                   -o $@ test_localio_board.c $(COMP)/clu_localio/localio.c $(COMP)/clu_i2c/clu_i2c.c \
                   $(PROTO)/tfbus_frame.c $(PROTO)/tfbus_vmod.c

$(OUT)/test_localio_f8: $(LOCALIO_BOARD_DEPS)
	$(LOCALIO_BOARD_CC) -DBOARD_F8=1

$(OUT)/test_localio_a8v3: $(LOCALIO_BOARD_DEPS)
	$(LOCALIO_BOARD_CC) -DBOARD_A8V3=1

# UDP command server (main/udp_server.c, included by the test) with fake sockets and cipher.
UDP_INC := -Istubs -I$(MAIN) -I$(PROTO)/include -I$(COMP)/clu_crypto/include -I$(LUA_DIR) \
           -I$(COMP)/clu_lua/include -I$(COMP)/tfbus_master/include -I$(COMP)/clu_tftp/include

$(OUT)/test_udp: test_udp.c $(MAIN)/udp_server.c $(PROTO)/clu_keys.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    $(UDP_INC) -o $@ test_udp.c $(PROTO)/clu_keys.c

# The same without a TF-Bus master (CLU_BUS_NONE / GATE): discovery and "Clear CLU" do not rescan.
$(OUT)/test_udp_nobus: test_udp.c $(MAIN)/udp_server.c $(PROTO)/clu_keys.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    $(UDP_INC) -DCONFIG_CLU_TFBUS=0 -o $@ test_udp.c $(PROTO)/clu_keys.c

# RS485 transport (tfbus_uart.c) against a fake ESP-IDF UART driver.
$(OUT)/test_uart: test_uart.c $(COMP)/tfbus_uart/tfbus_uart.c $(PROTO)/tfbus_frame.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) \
	    -Istubs -I$(PROTO)/include -I$(COMP)/tfbus_uart/include -o $@ $(filter %.c,$^)

# System helpers in main/ (network, time, identity/keys, diagnostics, sysinfo): each source is a
# separate object with sys_host.h force-included (/fs -> temp dir, CONFIG_CLU_* -> variables).
SYS_SRCS := $(MAIN)/clu_net.c $(MAIN)/clu_time.c $(MAIN)/clu_state.c $(MAIN)/clu_diag.c $(MAIN)/clu_sysinfo.c
SYS_OBJS := $(patsubst $(MAIN)/%.c,$(OUT)/sys_%.o,$(SYS_SRCS))
SYS_INC := -Istubs -I. -I$(MAIN) -I$(PROTO)/include -I$(COMP)/clu_crypto/include -I$(LUA_DIR) \
           -I$(COMP)/clu_lua/include -I$(COMP)/tfbus_master/include -I$(COMP)/clu_mqtt/include \
           -I$(COMP)/clu_rtc/include -I$(COMP)/clu_localio/include
HOST_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter $(OPT) -g $(SAN) $(LUA_DEFS)

$(OUT)/sys_%.o: $(MAIN)/%.c sys_host.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -include sys_host.h -c $< -o $@

$(OUT)/test_sys: test_sys.c $(SYS_OBJS) $(PROTO)/clu_keys.c $(LUALIB) fake_nvs.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -o $@ test_sys.c $(SYS_OBJS) $(PROTO)/clu_keys.c $(LUALIB) -lm -ldl

# Crash record (main/clu_crash.c, included by the test): panic wrapper, task watchdog hook, next boot.
$(OUT)/test_crash: test_crash.c $(MAIN)/clu_crash.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(MAIN) -o $@ test_crash.c

# TFTP server (clu_tftp.c, included by the test) against a reactive fake peer, files in /tmp.
$(OUT)/test_tftp: test_tftp.c $(COMP)/clu_tftp/clu_tftp.c $(PROTO)/tftp_proto.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(PROTO)/include -I$(COMP)/clu_tftp/include \
	    -o $@ test_tftp.c $(PROTO)/tftp_proto.c

# Telnet log console (clu_telnet.c, included by the test) against fake TCP sockets.
$(OUT)/test_telnet: test_telnet.c $(COMP)/clu_telnet/clu_telnet.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_telnet/include -o $@ test_telnet.c

# Network updates (clu_ota.c, included by the test) against a fake HTTP server and OTA API.
$(OUT)/test_ota: test_ota.c $(COMP)/clu_ota/clu_ota.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_ota/include -o $@ test_ota.c

# MQTT link (clu_mqtt.c, included by the test) against a fake esp-mqtt client.
$(OUT)/test_mqttc: test_mqttc.c $(COMP)/clu_mqtt/clu_mqtt.c $(LUALIB) $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(LUA_DIR) -I$(COMP)/clu_lua/include -I$(COMP)/clu_mqtt/include \
	    -o $@ test_mqttc.c $(LUALIB) -lm -ldl

# GATE HTTP objects, C side (clu_http.c, included by the test): Lua bindings in a real VM, the
# worker against a fake esp_http_client, the listener against fake sockets; http_proto.c linked.
$(OUT)/test_httpc: test_httpc.c $(COMP)/clu_http/clu_http.c $(PROTO)/http_proto.c $(LUALIB) $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(PROTO)/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include -I$(COMP)/clu_http/include \
	    -o $@ test_httpc.c $(PROTO)/http_proto.c $(LUALIB) -lm -ldl

# MySensors protocol (ms_proto.c) and controller (clu_mysensors.c, included by the test) against a
# fake UART, NVS and clock; the Lua bindings in a real VM.
MS_SRCS := $(COMP)/clu_mysensors/ms_vmod.c $(PROTO)/ms_proto.c $(PROTO)/ms_rs485.c $(PROTO)/tfbus_vmod.c $(PROTO)/tfbus_frame.c
$(OUT)/test_mysensors: test_mysensors.c $(COMP)/clu_mysensors/clu_mysensors.c $(COMP)/clu_mysensors/ms_vmod.h $(MS_SRCS) \
                $(LUALIB) fake_nvs.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Wno-unused-function -Istubs -I. -I$(PROTO)/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include \
	    -I$(COMP)/clu_mysensors/include -I$(COMP)/clu_mysensors -I$(COMP)/tfbus_uart/include \
	    -o $@ test_mysensors.c $(MS_SRCS) $(LUALIB) -lm -ldl

# Lua runtime (clu_lua.c, included by the test; prelude.lua / objects.lua embedded with .incbin)
# with the Lua task on a pthread.
$(OUT)/test_lua: test_lua.c $(COMP)/clu_lua/clu_lua.c $(COMP)/clu_lua/prelude.lua $(COMP)/clu_lua/objects.lua \
          $(LUALIB) $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(LUA_DIR) -I$(COMP)/clu_lua/include -o $@ test_lua.c $(LUALIB) -lm -ldl -lpthread

# RTC (clu_rtc.c + clu_i2c.c included, pcf85063.c / ds3231.c linked) against fake chips on a fake I2C bus.
$(OUT)/test_rtc: test_rtc.c $(COMP)/clu_rtc/clu_rtc.c $(COMP)/clu_rtc/pcf85063.c $(COMP)/clu_rtc/ds3231.c \
          $(COMP)/clu_i2c/clu_i2c.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_rtc/include -I$(COMP)/clu_i2c/include -o $@ test_rtc.c \
	    $(COMP)/clu_rtc/pcf85063.c $(COMP)/clu_rtc/ds3231.c

# OLED text rows (display_text.c): 5x7 glyphs in SSD1306 pages.
$(OUT)/test_display: test_display.c $(COMP)/clu_display/display_text.c $(HDRS)
	$(TCC) $(CFLAGS) -I$(COMP)/clu_display/include -o $@ $(filter %.c,$^)

# Status OLED: SSD1306 glue (clu_display.c + clu_i2c.c) against a fake esp_lcd panel, and the status
# screen (main/clu_status_display.c) against fake network, clock and TF-Bus registry; all included.
$(OUT)/test_oled: test_oled.c $(COMP)/clu_display/clu_display.c $(COMP)/clu_display/display_text.c \
                  $(COMP)/clu_i2c/clu_i2c.c $(MAIN)/clu_status_display.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(MAIN) -I$(PROTO)/include -I$(LUA_DIR) -I$(COMP)/clu_display/include \
	    -I$(COMP)/clu_i2c/include -I$(COMP)/clu_crypto/include -I$(COMP)/clu_rtc/include \
	    -I$(COMP)/tfbus_master/include -o $@ test_oled.c $(COMP)/clu_display/display_text.c

# clu_time.c with CLU_RTC off (the RTC driver must never be called).
$(OUT)/nortc_clu_time.o: $(MAIN)/clu_time.c sys_host.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -DCONFIG_CLU_RTC=0 -include sys_host.h -c $< -o $@

$(OUT)/test_time_nortc: test_time_nortc.c $(OUT)/nortc_clu_time.o fake_nvs.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -o $@ test_time_nortc.c $(OUT)/nortc_clu_time.o

# Boot wiring (main.c, included by the test): every module / ESP-IDF call stubbed and logged.
MAIN_INC := -Istubs -I$(MAIN) -I$(PROTO)/include -I$(LUA_DIR) $(addprefix -I$(COMP)/,$(addsuffix /include,\
            clu_crypto clu_lua clu_mqtt clu_ota clu_telnet clu_tftp tfbus_drivers tfbus_master tfbus_uart))
$(OUT)/test_main: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -o $@ test_main.c -lm

# The same with CLU_TFBUS off: no UART, drivers or bus master.
$(OUT)/test_main_nobus: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -DCONFIG_CLU_TFBUS=0 -o $@ test_main.c -lm

# The same with the MySensors gateway on the bus pins instead (imperial units, up to two emulated
# IO_MODULE_DIN_8): the bus master runs without a wire.
$(OUT)/test_main_mysensors: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -I$(COMP)/clu_mysensors/include -DCONFIG_CLU_TFBUS=0 -DCONFIG_CLU_MYSENSORS=1 \
	    -DCONFIG_CLU_MYSENSORS_MODULES_MAX=2 -o $@ test_main.c -lm

# The same as a GATE HTTP: no bus, GATE feature indices, HTTP objects.
$(OUT)/test_main_gate: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -I$(COMP)/clu_http/include -DCONFIG_CLU_TFBUS=0 -DCONFIG_CLU_DEVICE_GATE_HTTP=1 \
	    -DCONFIG_CLU_HTTP=1 -o $@ test_main.c -lm

# Ethernet (main/clu_eth.c, included by the test): internal EMAC + LAN8720A, and the W5500 on SPI.
ETH_CC = $(TCC) $(HOST_CFLAGS) -Istubs -I$(MAIN) -o $@ test_eth.c
$(OUT)/test_eth_emac: test_eth.c $(MAIN)/clu_eth.c $(STUBS) $(HDRS)
	$(ETH_CC)

$(OUT)/test_eth_w5500: test_eth.c $(MAIN)/clu_eth.c $(STUBS) $(HDRS)
	$(ETH_CC) -DCONFIG_CLU_ETH_IF_W5500=1

# AES (clu_crypto.c) against the host's mbedtls 3.6 PSA API (Arch: mbedtls3; elsewhere set
# MBEDTLS3_INC / MBEDTLS3_LIB). Skipped by `make test` when the headers are missing.
MBEDTLS3_INC ?= /usr/include/mbedtls3
MBEDTLS3_LIB ?= /usr/lib/mbedtls3
$(OUT)/test_crypto: test_crypto.c $(COMP)/clu_crypto/clu_crypto.c $(PROTO)/clu_keys.c
	$(TCC) -std=gnu11 -Wall -Wextra -Werror $(OPT) -g $(SAN) \
	    -Istubs -I$(MBEDTLS3_INC) -I$(PROTO)/include -I$(COMP)/clu_crypto/include \
	    -o $@ $^ $(MBEDTLS3_LIB)/libmbedcrypto.a

HAVE_MBEDTLS3 := $(wildcard $(MBEDTLS3_INC)/psa/crypto.h)

C_TESTS := test_proto test_drivers test_master test_udp test_udp_nobus test_uart test_localio test_localio_sensors test_localio_hw test_localio_f8 test_localio_a8v3 test_sys test_crash test_tftp test_telnet test_ota test_mqttc test_httpc test_mysensors test_lua test_main test_main_nobus test_main_mysensors test_main_gate test_time_nortc test_rtc test_display test_oled test_eth_emac test_eth_w5500
C_TESTS := $(addprefix $(OUT)/,$(C_TESTS) $(if $(HAVE_MBEDTLS3),test_crypto))

.PHONY: test test-c clean coverage
test: test-c test-lua

test-c: $(C_TESTS)
	$(OUT)/test_proto
	$(OUT)/test_drivers
	$(OUT)/test_master 2>/dev/null
	$(OUT)/test_udp 2>/dev/null
	$(OUT)/test_udp_nobus 2>/dev/null
	$(OUT)/test_rtc 2>/dev/null
	$(OUT)/test_display
	$(OUT)/test_oled 2>/dev/null
	$(OUT)/test_eth_emac 2>/dev/null
	$(OUT)/test_eth_w5500 2>/dev/null
	$(OUT)/test_uart 2>/dev/null
	$(OUT)/test_localio
	$(OUT)/test_localio_sensors
	$(OUT)/test_localio_hw 2>/dev/null
	$(OUT)/test_localio_f8 2>/dev/null
	$(OUT)/test_localio_a8v3 2>/dev/null
	$(OUT)/test_sys 2>/dev/null
	$(OUT)/test_crash 2>/dev/null
	$(OUT)/test_tftp 2>/dev/null
	$(OUT)/test_telnet 2>/dev/null
	$(OUT)/test_ota 2>/dev/null
	$(OUT)/test_mqttc 2>/dev/null
	$(OUT)/test_httpc 2>/dev/null
	$(OUT)/test_mysensors 2>/dev/null
	$(OUT)/test_lua 2>/dev/null
	$(OUT)/test_main 2>/dev/null
	$(OUT)/test_main_nobus 2>/dev/null
	$(OUT)/test_main_mysensors 2>/dev/null
	$(OUT)/test_main_gate 2>/dev/null
	$(OUT)/test_time_nortc 2>/dev/null
	$(if $(HAVE_MBEDTLS3),$(OUT)/test_crypto,@echo "skip test_crypto: no $(MBEDTLS3_INC)/psa/crypto.h")

clean:
	rm -rf $(OUT) && mkdir -p $(OUT) # recreated: `make clean test` reuses the parse-time mkdir

# Prelude (Lua object model) + MQTT/Home Assistant (clu_mqtt/mqtt.lua) tests; needs lua$(LUA) on the host. Pass OM=<path> to also load a real OM.LUA.
# With COVERAGE=1 each script runs under lua_cov.lua (line hits -> $(OUT)/coverage/lua.hits).
LUA_RUN := $(if $(COV),lua$(LUA) -e "COVOUT='$(OUT)/coverage/lua.hits'" lua_cov.lua,lua$(LUA))
.PHONY: test-lua
test-lua:
	$(if $(COV),mkdir -p $(OUT)/coverage)
	$(LUA_RUN) test_prelude.lua
	$(LUA_RUN) test_mqtt.lua
	$(LUA_RUN) test_mysensors.lua
	$(LUA_RUN) test_http.lua
	$(if $(OM),$(LUA_RUN) test_prelude.lua "$(OM)")

# Event coverage: every event the supported OM interfaces declare is fired or listed as missing
# (check_events.py). The interface files are not in the repository: make test-events INTERFACES=<dir>.
.PHONY: test-events
test-events:
	$(if $(INTERFACES),python3 check_events.py --report "$(INTERFACES)",$(error INTERFACES=<device-interfaces dir> not set))

# Line/branch coverage of C (gcov) and Lua (lua_cov.lua) code under test: summary on stdout,
# $(OUT)/coverage/summary.md (Markdown table) and $(OUT)/coverage/coverage.json. Needs gcc/gcov, python3.
# FAIL_UNDER=<pct> fails the run when total line coverage is lower.
coverage:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 test
	python3 coverage.py --luac luac$(LUA) $(if $(FAIL_UNDER),--fail-under $(FAIL_UNDER))
