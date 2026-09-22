# Abspielen der Treiberlogik gegen einen echten Speicherabzug.
# Braucht keinen Kernel und keine Hardware.
#
#   make && ./tm_replay mvbsnap
#
# mvbsnap/ ist ein mit tools/mvbsnap.py erzeugtes Verzeichnis mit
# tm_low.bin und tm_high.bin.

CFLAGS = -I fakelinux -I ../recipes-kernel/pixy-mvblli/files -Wall -O1 -g

tm_replay: tm_replay.c ../recipes-kernel/pixy-mvblli/files/pixy-mvblli.c
	$(CC) $(CFLAGS) -o $@ tm_replay.c

check: tm_replay
	./tm_replay mvbsnap

clean:
	rm -f tm_replay

.PHONY: check clean
