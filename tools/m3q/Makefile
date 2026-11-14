# M3q：宿主机 shader 翻译/编译工具（离线；不进 HAP）
CPP := ../../mario-odessy-harmony/entry/src/main/cpp
CC  ?= clang
CFLAGS := -O2 -std=c11 -Wall -Wextra -I$(CPP)

m3q_translate: m3q_translate.c $(CPP)/platform/nvn_replay/maxwell/translate.c
	$(CC) $(CFLAGS) -o $@ m3q_translate.c $(CPP)/platform/nvn_replay/maxwell/translate.c -lm

clean:
	rm -f m3q_translate
