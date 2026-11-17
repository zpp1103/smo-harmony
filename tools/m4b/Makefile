# M4b：配对重烘工具（宿主侧离线；不进 HAP）
CPP := ../../mario-odessy-harmony/entry/src/main/cpp
CC  ?= clang
CFLAGS := -O2 -std=c11 -Wall -Wextra -I$(CPP)

pairbake: pairbake.c $(CPP)/platform/nvn_replay/maxwell/translate.c
	$(CC) $(CFLAGS) -o $@ pairbake.c $(CPP)/platform/nvn_replay/maxwell/translate.c -lm

clean:
	rm -f pairbake
