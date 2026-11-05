# Host (PC) unit tests for the pure-C diagnostic logic.
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
BUILD := build
SRC := ../../main/diag_logic.c test_diag_logic.c

all: run

$(BUILD)/test_diag_logic: $(SRC) ../../main/diag_logic.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../../main -o $@ $(SRC)

run: $(BUILD)/test_diag_logic
	./$(BUILD)/test_diag_logic

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
