# Abspielen der Treiberlogik gegen einen echten Speicherabzug.
# Braucht keinen Kernel und keine Hardware.
#
#   make && ./tm_replay mvbsnap
#
# mvbsnap/ ist ein mit tools/mvbsnap.py erzeugtes Verzeichnis mit
# tm_low.bin und tm_high.bin.

CFLAGS = -I fakelinux -I ../recipes-kernel/pixy-mvblli/files -Wall -O1 -g
SRC    = tm_replay.c ../recipes-kernel/pixy-mvblli/files/pixy-mvblli.c

# Der Treiber waehlt an drei Stellen per LINUX_VERSION_CODE zwischen der
# Schreibweise fuer 5.10 und der fuer 6.x. Beide Zweige werden uebersetzt
# und beide muessen dieselben Pruefungen bestehen - siehe
# fakelinux/linux/version.h.
V6 = -DFAKE_KERNEL_CODE=KERNEL_VERSION\(6,12,0\)

tm_replay: $(SRC)
	$(CC) $(CFLAGS) -o $@ tm_replay.c

tm_replay-6x: $(SRC)
	$(CC) $(CFLAGS) $(V6) -o $@ tm_replay.c

check: tm_replay
	./tm_replay mvbsnap

check-6x: tm_replay-6x
	./tm_replay-6x mvbsnap

# Beide Kernelstaende hintereinander
check-all: check check-6x

clean:
	rm -f tm_replay tm_replay-6x

.PHONY: check check-6x check-all clean
