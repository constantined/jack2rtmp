CC_OPTS = -Wall -lev -ljack -lmp3lame -lrtmp

all: bin/jack2rtmp

clean:
	rm -f bin/*

bin/jack2rtmp: src/jack2rtmp.c
	$(CC) $(CC_OPTS) $< -o $@

