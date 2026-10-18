SDK_CC ?= /opt/ps5-payload-sdk/bin/prospero-clang
HOST_CC ?= clang
SDK_INCLUDE = -I../../sdk-13_60/include
SDL_CFLAGS = -I/opt/ps5-payload-sdk/target/user/homebrew/include/SDL2
SDL_LIBS = /opt/ps5-payload-sdk/target/user/homebrew/lib/libSDL2_gfx.a \
    /opt/ps5-payload-sdk/target/user/homebrew/lib/libSDL2.a -liconv \
    -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lsamplerate \
    -lSceVideoOut -lSceKeyboard -lSceImeDialog
SERVICE_CFLAGS = -std=c11 -Wall -Wextra -Werror -O2 -ffreestanding -fpie \
    -mcmodel=small -mno-red-zone -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin \
    -fno-jump-tables -mgeneral-regs-only -fvisibility=hidden \
    -fno-semantic-interposition -ffunction-sections -fdata-sections $(SDK_INCLUDE)

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

all: task_gateway.bin controller_gateway.bin ps5_emc_log_reader_test_1360.elf emc_ui_smoke.elf

standalone: task_gateway.bin controller_gateway.bin ps5_emc_log_reader_standalone_1360.elf

controller_service.o: controller_service.c task_probe.h
	$(SDK_CC) $(SERVICE_CFLAGS) -c -o $@ $<

controller_service.elf: controller_service.o flat.ld
	ld.lld -pie -nostdlib --gc-sections -T flat.ld -e tg_controller_service \
	    -o $@ controller_service.o
	@test -z "$$(llvm-nm --undefined-only --just-symbol-name $@)"
	@test "$$(llvm-readelf -r $@ | grep -c 'R_X86_64_')" -eq 0

controller_service.bin: controller_service.elf
	llvm-objcopy -O binary --only-section=.text $< $@
	@test $$(stat -c%s $@) -le 3072

task_gateway.o: task_gateway.S task_probe.h
	$(SDK_CC) -c -o $@ $<

task_gateway.elf: task_gateway.o
	ld -nostdlib -Ttext=0 -e tg_task_gateway $< -o $@
	@test "$$(llvm-readelf -r $@ | grep -c 'R_X86_64_')" -eq 0

task_gateway.bin: task_gateway.elf
	llvm-objcopy -O binary --only-section=.text $< $@
	@test $$(stat -c%s $@) -eq 3936

controller_gateway.o: controller_gateway.S task_probe.h controller_service.bin
	$(SDK_CC) -c -o $@ $<

controller_gateway.elf: controller_gateway.o
	ld -nostdlib -Ttext=0 -e tg_controller_gateway $< -o $@
	@test "$$(llvm-readelf -r $@ | grep -c 'R_X86_64_')" -eq 0

controller_gateway.bin: controller_gateway.elf
	llvm-objcopy -O binary --only-section=.text $< $@
	@test $$(stat -c%s $@) -eq 3904

ps5_emc_log_reader_test_1360.elf: main.c task_probe.h images.S \
        task_gateway.bin controller_gateway.bin
	$(SDK_CC) -std=c11 -Wall -Wextra -Werror -O2 -pthread $(SDL_CFLAGS) \
	    -o $@ main.c images.S /opt/ps5-payload-sdk/target/lib/libps5fw1360.a \
	    $(SDL_LIBS)

ps5_emc_log_reader_standalone_1360.elf: main.c task_probe.h images.S \
        task_gateway.bin controller_gateway.bin
	$(SDK_CC) -std=c11 -Wall -Wextra -Werror -O2 -pthread -DTG_STANDALONE \
	    -o $@ main.c images.S \
	    /opt/ps5-payload-sdk/target/lib/libps5fw1360.a

emc_ui_smoke.elf: ui_smoke.c
	$(SDK_CC) -std=c11 -Wall -Wextra -Werror -O2 $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS)

host-test: all tests/host_test.c tests/service_host_test.c controller_service.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g -pthread \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    tests/host_test.c -o host-test
	./host-test task_gateway.bin controller_gateway.bin
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g -pthread \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -DTG_HOST_TEST \
	    tests/service_host_test.c controller_service.c -o service-host-test
	./service-host-test task_gateway.bin
	python3 -m unittest discover -s tests -p 'test_protocol.py' -v

clean:
	rm -f *.o *.elf *.bin host-test service-host-test

.PHONY: all standalone host-test clean
