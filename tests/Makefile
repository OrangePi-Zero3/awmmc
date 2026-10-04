# Host-side tests for the SMHC miniport.  Not a WDK build: see README.md.
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=address,undefined
INC     := -Imock -I../driver
DRV     := ../driver/smhc.c ../driver/smhc_hw.c

all: test_core test_flow

test_core: test_core.c ../driver/smhc_core.h ../driver/smhc_regs.h mock/*.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_core.c

test_flow: test_flow.c emu.c emu.h $(DRV) ../driver/*.h mock/*.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_flow.c emu.c $(DRV)

check: all
	./test_core
	./test_flow

syntax:
	$(CC) -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-unused-parameter $(INC) $(DRV)

clean:
	rm -f test_core test_flow
