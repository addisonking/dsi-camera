CC = clang
CFLAGS = -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
ROOT = ../..

.PHONY: test

test:
	$(CC) $(CFLAGS) -I$(ROOT)/arm9/include $(ROOT)/arm9/source/night.c night_test.c -o /tmp/dsi-camera-night-test
	/tmp/dsi-camera-night-test
	$(CC) $(CFLAGS) -Wno-unused-parameter -Ishim -I$(ROOT)/arm7/include -I$(ROOT)/shared/include $(ROOT)/arm7/source/aptina.c exposure_test.c -o /tmp/dsi-camera-exposure-test
	/tmp/dsi-camera-exposure-test
	$(CC) $(CFLAGS) -Wno-unused-parameter -Ishim -I$(ROOT)/arm7/include -I$(ROOT)/shared/include $(ROOT)/arm7/source/aptina.c rear_light_test.c -o /tmp/dsi-camera-rear-light-test
	/tmp/dsi-camera-rear-light-test
