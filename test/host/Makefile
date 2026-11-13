PROTO := ../../components/clu_proto
COMP := ../../components
MAIN := ../../main

# make COVERAGE=1 ...: instrument the code under test (gcov); `make coverage` does the whole run.
COVERAGE ?= 0
COV := $(if $(filter 1,$(COVERAGE)),--coverage)
TCC := $(CC) $(COV)
SAN := -fsanitize=address,undefined
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g $(SAN) -I$(PROTO)/include
# Tests compile against these stubs and headers: rebuild when they change.
STUBS := $(wildcard stubs/*.h stubs/*/*.h)
HDRS := $(wildcard $(COMP)/*/include/*.h $(COMP)/tfbus_drivers/*.h $(MAIN)/*.h)

test_proto: test_proto.c $(PROTO)/tfbus_frame.c $(PROTO)/clu_keys.c $(PROTO)/tftp_proto.c $(HDRS)
	$(TCC) $(CFLAGS) -o $@ $(filter %.c,$^)

# Lua version under test: make LUA=5.5 clean test
LUA ?= 5.3
LUA_DIR := $(COMP)/lua/src-$(LUA)
LUA_DEFS := $(if $(filter 5.3,$(LUA)),-DLUA_COMPAT_5_2) -DLUAI_MAXCCALLS=120 -DMAXCCALLS=120 # = components/lua/CMakeLists.txt
LUA_SRCS := $(filter-out %/lua.c %/luac.c,$(wildcard $(LUA_DIR)/*.c))
DRV_SRCS := $(wildcard $(COMP)/tfbus_drivers/*.c)
DRV_INC := -Istubs -I$(COMP)/clu_proto/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include \
           -I$(COMP)/tfbus_master/include -I$(COMP)/tfbus_drivers/include

# The Lua VM itself is not instrumented (third-party, not under test).
liblua_host.a: $(LUA_SRCS)
	rm -f $@ *.o
	$(CC) -std=gnu99 -O1 -g $(SAN) $(LUA_DEFS) -DLUA_USE_LINUX -c $(LUA_SRCS)
	ar rcs $@ *.o && rm -f *.o

test_drivers: test_drivers.c $(DRV_SRCS) $(PROTO)/tfbus_frame.c liblua_host.a $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g $(SAN) \
	    $(LUA_DEFS) $(DRV_INC) -o $@ test_drivers.c $(DRV_SRCS) $(PROTO)/tfbus_frame.c liblua_host.a -lm -ldl

MASTER_INC := -Istubs -I$(COMP)/clu_proto/include -I$(LUA_DIR) -I$(COMP)/clu_lua/include \
              -I$(COMP)/tfbus_master/include -I$(COMP)/tfbus_uart/include

test_master: test_master.c $(COMP)/tfbus_master/tfbus_master.c $(PROTO)/tfbus_frame.c liblua_host.a $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-but-set-variable -O1 -g $(SAN) \
	    $(LUA_DEFS) $(MASTER_INC) -o $@ test_master.c $(PROTO)/tfbus_frame.c liblua_host.a -lm -ldl

# On-board IO emulation (two IO_MODULE_DIN_8 answering TF-Bus frames) against fake expanders.
test_localio: test_localio.c $(COMP)/clu_localio/localio.c $(PROTO)/tfbus_frame.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g $(SAN) \
	    -Istubs -I$(PROTO)/include -I$(COMP)/clu_localio/include -o $@ $(filter %.c,$^)

# On-board IO board glue (clu_localio.c, included by the test): serial in NVS, PCF8575 pin mapping
# of KinCony F8 and KC868-A8v3, against a fake expander.
LOCALIO_BOARD_DEPS := test_localio_board.c $(COMP)/clu_localio/clu_localio.c $(COMP)/clu_localio/localio.c \
                      $(COMP)/clu_i2c/clu_i2c.c $(PROTO)/tfbus_frame.c fake_nvs.h $(STUBS) $(HDRS)
LOCALIO_BOARD_CC = $(TCC) $(HOST_CFLAGS) -Wno-unused-function -Istubs -I. -I$(PROTO)/include -I$(COMP)/clu_localio/include \
                   -I$(COMP)/clu_crypto/include -I$(COMP)/tfbus_uart/include -I$(COMP)/clu_i2c/include \
                   -o $@ test_localio_board.c $(COMP)/clu_localio/localio.c $(COMP)/clu_i2c/clu_i2c.c \
                   $(PROTO)/tfbus_frame.c

test_localio_f8: $(LOCALIO_BOARD_DEPS)
	$(LOCALIO_BOARD_CC) -DBOARD_F8=1

test_localio_a8v3: $(LOCALIO_BOARD_DEPS)
	$(LOCALIO_BOARD_CC) -DBOARD_A8V3=1

# UDP command server (main/udp_server.c, included by the test) with fake sockets and cipher.
UDP_INC := -Istubs -I$(MAIN) -I$(PROTO)/include -I$(COMP)/clu_crypto/include -I$(LUA_DIR) \
           -I$(COMP)/clu_lua/include -I$(COMP)/tfbus_master/include -I$(COMP)/clu_tftp/include

test_udp: test_udp.c $(MAIN)/udp_server.c $(PROTO)/clu_keys.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g $(SAN) \
	    $(UDP_INC) -o $@ test_udp.c $(PROTO)/clu_keys.c

# RS485 transport (tfbus_uart.c) against a fake ESP-IDF UART driver.
test_uart: test_uart.c $(COMP)/tfbus_uart/tfbus_uart.c $(PROTO)/tfbus_frame.c $(STUBS) $(HDRS)
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g $(SAN) \
	    -Istubs -I$(PROTO)/include -I$(COMP)/tfbus_uart/include -o $@ $(filter %.c,$^)

# System helpers in main/ (network, time, identity/keys, diagnostics, sysinfo): each source is a
# separate object with sys_host.h force-included (/fs -> temp dir, CONFIG_CLU_* -> variables).
SYS_SRCS := $(MAIN)/clu_net.c $(MAIN)/clu_time.c $(MAIN)/clu_state.c $(MAIN)/clu_diag.c $(MAIN)/clu_sysinfo.c
SYS_OBJS := $(patsubst $(MAIN)/%.c,sys_%.o,$(SYS_SRCS))
SYS_INC := -Istubs -I. -I$(MAIN) -I$(PROTO)/include -I$(COMP)/clu_crypto/include -I$(LUA_DIR) \
           -I$(COMP)/clu_lua/include -I$(COMP)/tfbus_master/include -I$(COMP)/clu_mqtt/include \
           -I$(COMP)/clu_rtc/include
HOST_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g $(SAN) $(LUA_DEFS)

sys_%.o: $(MAIN)/%.c sys_host.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -include sys_host.h -c $< -o $@

test_sys: test_sys.c $(SYS_OBJS) $(PROTO)/clu_keys.c liblua_host.a fake_nvs.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -o $@ test_sys.c $(SYS_OBJS) $(PROTO)/clu_keys.c liblua_host.a -lm -ldl

# TFTP server (clu_tftp.c, included by the test) against a reactive fake peer, files in /tmp.
test_tftp: test_tftp.c $(COMP)/clu_tftp/clu_tftp.c $(PROTO)/tftp_proto.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(PROTO)/include -I$(COMP)/clu_tftp/include \
	    -o $@ test_tftp.c $(PROTO)/tftp_proto.c

# Telnet log console (clu_telnet.c, included by the test) against fake TCP sockets.
test_telnet: test_telnet.c $(COMP)/clu_telnet/clu_telnet.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_telnet/include -o $@ test_telnet.c

# Network updates (clu_ota.c, included by the test) against a fake HTTP server and OTA API.
test_ota: test_ota.c $(COMP)/clu_ota/clu_ota.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_ota/include -o $@ test_ota.c

# MQTT link (clu_mqtt.c, included by the test) against a fake esp-mqtt client.
test_mqttc: test_mqttc.c $(COMP)/clu_mqtt/clu_mqtt.c liblua_host.a $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(LUA_DIR) -I$(COMP)/clu_lua/include -I$(COMP)/clu_mqtt/include \
	    -o $@ test_mqttc.c liblua_host.a -lm -ldl

# Lua runtime (clu_lua.c, included by the test; prelude.lua / objects.lua embedded with .incbin)
# with the Lua task on a pthread.
test_lua: test_lua.c $(COMP)/clu_lua/clu_lua.c $(COMP)/clu_lua/prelude.lua $(COMP)/clu_lua/objects.lua \
          liblua_host.a $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(LUA_DIR) -I$(COMP)/clu_lua/include -o $@ test_lua.c liblua_host.a -lm -ldl -lpthread

# RTC (clu_rtc.c + clu_i2c.c included, pcf85063.c / ds3231.c linked) against fake chips on a fake I2C bus.
test_rtc: test_rtc.c $(COMP)/clu_rtc/clu_rtc.c $(COMP)/clu_rtc/pcf85063.c $(COMP)/clu_rtc/ds3231.c \
          $(COMP)/clu_i2c/clu_i2c.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) -Istubs -I$(COMP)/clu_rtc/include -I$(COMP)/clu_i2c/include -o $@ test_rtc.c \
	    $(COMP)/clu_rtc/pcf85063.c $(COMP)/clu_rtc/ds3231.c

# clu_time.c with CLU_RTC off (the RTC driver must never be called).
nortc_clu_time.o: $(MAIN)/clu_time.c sys_host.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -DCONFIG_CLU_RTC=0 -include sys_host.h -c $< -o $@

test_time_nortc: test_time_nortc.c nortc_clu_time.o fake_nvs.h $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(SYS_INC) -o $@ test_time_nortc.c nortc_clu_time.o

# Boot wiring (main.c, included by the test): every module / ESP-IDF call stubbed and logged.
MAIN_INC := -Istubs -I$(MAIN) -I$(PROTO)/include -I$(LUA_DIR) $(addprefix -I$(COMP)/,$(addsuffix /include,\
            clu_crypto clu_lua clu_mqtt clu_ota clu_telnet clu_tftp tfbus_drivers tfbus_master tfbus_uart))
test_main: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -o $@ test_main.c -lm

# The same with CLU_TFBUS off: no UART, drivers or bus master.
test_main_nobus: test_main.c $(MAIN)/main.c $(STUBS) $(HDRS)
	$(TCC) $(HOST_CFLAGS) $(MAIN_INC) -DCONFIG_CLU_TFBUS=0 -o $@ test_main.c -lm

# AES (clu_crypto.c) against the host's mbedtls 3.6 PSA API (Arch: mbedtls3; elsewhere set
# MBEDTLS3_INC / MBEDTLS3_LIB). Skipped by `make test` when the headers are missing.
MBEDTLS3_INC ?= /usr/include/mbedtls3
MBEDTLS3_LIB ?= /usr/lib/mbedtls3
test_crypto: test_crypto.c $(COMP)/clu_crypto/clu_crypto.c $(PROTO)/clu_keys.c
	$(TCC) -std=gnu11 -Wall -Wextra -Werror -O1 -g $(SAN) \
	    -Istubs -I$(MBEDTLS3_INC) -I$(PROTO)/include -I$(COMP)/clu_crypto/include \
	    -o $@ $^ $(MBEDTLS3_LIB)/libmbedcrypto.a

HAVE_MBEDTLS3 := $(wildcard $(MBEDTLS3_INC)/psa/crypto.h)

C_TESTS := test_proto test_drivers test_master test_udp test_uart test_localio test_localio_f8 test_localio_a8v3 test_sys test_tftp test_telnet test_ota test_mqttc test_lua test_main test_main_nobus test_time_nortc test_rtc $(if $(HAVE_MBEDTLS3),test_crypto)

.PHONY: test test-c clean coverage
test: test-c test-lua

test-c: $(C_TESTS)
	./test_proto
	ASAN_OPTIONS=detect_leaks=0 ./test_drivers
	ASAN_OPTIONS=detect_leaks=0 ./test_master 2>/dev/null
	./test_udp 2>/dev/null
	./test_rtc 2>/dev/null
	./test_uart 2>/dev/null
	./test_localio
	./test_localio_f8 2>/dev/null
	./test_localio_a8v3 2>/dev/null
	ASAN_OPTIONS=detect_leaks=0 ./test_sys 2>/dev/null
	./test_tftp 2>/dev/null
	./test_telnet 2>/dev/null
	./test_ota 2>/dev/null
	ASAN_OPTIONS=detect_leaks=0 ./test_mqttc 2>/dev/null
	ASAN_OPTIONS=detect_leaks=0 ./test_lua 2>/dev/null
	./test_main 2>/dev/null
	./test_main_nobus 2>/dev/null
	./test_time_nortc 2>/dev/null
	$(if $(HAVE_MBEDTLS3),./test_crypto,@echo "skip test_crypto: no $(MBEDTLS3_INC)/psa/crypto.h")

clean:
	rm -f test_proto test_drivers test_master test_udp test_uart test_localio test_localio_f8 test_localio_a8v3 test_sys test_tftp test_telnet test_ota test_mqttc test_lua test_main test_main_nobus test_time_nortc test_rtc test_crypto liblua_host.a *.o
	rm -rf *.gcda *.gcno coverage

# Prelude (Lua object model) + MQTT/Home Assistant (clu_mqtt/mqtt.lua) tests; needs lua$(LUA) on the host. Pass OM=<path> to also load a real OM.LUA.
# With COVERAGE=1 each script runs under lua_cov.lua (line hits -> coverage/lua.hits).
LUA_RUN := $(if $(COV),lua$(LUA) -e "COVOUT='coverage/lua.hits'" lua_cov.lua,lua$(LUA))
.PHONY: test-lua
test-lua:
	$(if $(COV),mkdir -p coverage)
	$(LUA_RUN) test_prelude.lua
	$(LUA_RUN) test_mqtt.lua
	$(if $(OM),$(LUA_RUN) test_prelude.lua "$(OM)")

# Line/branch coverage of C (gcov) and Lua (lua_cov.lua) code under test: summary on stdout,
# coverage/summary.md (Markdown table) and coverage/coverage.json. Needs gcc/gcov, python3.
# FAIL_UNDER=<pct> fails the run when total line coverage is lower.
coverage:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 test
	python3 coverage.py --luac luac$(LUA) $(if $(FAIL_UNDER),--fail-under $(FAIL_UNDER))
